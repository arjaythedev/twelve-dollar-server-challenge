"""PostStore on one in-process SQLite connection. SQLite renders every response document.

Group commit: the first write in an event-loop pass opens a transaction (BEGIN IMMEDIATE) and
schedules one COMMIT with loop.call_soon. Every write made in that pass joins the transaction and
its response awaits the batch's future, so nothing is acknowledged before it is durable. Reads made
while a batch is open may have seen its uncommitted rows, so they await the same future.
"""

import asyncio
import sqlite3
from dataclasses import dataclass

from domain.errors import StoreUnavailable
from domain.models import Document, LikeOutcome, Principal

_POST_JSON = (
    "json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username,"
    " 'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id))"
)
# ORDER BY inside group_concat needs SQLite >= 3.44 (Ubuntu 24.04 ships 3.45.1).
FEED_SQL = (
    "SELECT '{\"posts\":[' || coalesce(group_concat(doc, ',' ORDER BY created_at DESC, id DESC), '') || ']}'"
    " FROM (SELECT " + _POST_JSON + " AS doc, p.created_at, p.id"
    " FROM posts p JOIN users u ON u.id = p.user_id"
    " ORDER BY p.created_at DESC, p.id DESC LIMIT 20)"
)
POST_SQL = (
    "SELECT '{\"post\":' || " + _POST_JSON + " || '}'"
    " FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?"
)
CREATE_SQL = (
    "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING '{\"post\":' || json_object("
    "'id', id, 'body', body, 'created_at', created_at, 'author', ?3, 'like_count', 0) || '}'"
)
LIKE_SQL = (
    "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)"
    " ON CONFLICT (user_id, post_id) DO NOTHING"
)
EXISTS_SQL = "SELECT 1 FROM posts WHERE id = ?"

# locking_mode=EXCLUSIVE must come before journal_mode=WAL so the WAL index lives in heap memory
# instead of the -shm file.
PRAGMAS = (
    "PRAGMA locking_mode = EXCLUSIVE",
    "PRAGMA journal_mode = WAL",
    "PRAGMA synchronous = NORMAL",  # durable on commit in WAL mode, per the rules
    "PRAGMA wal_autocheckpoint = 0",  # checkpoints run on a timer, off the commit path
    "PRAGMA mmap_size = 1073741824",
    "PRAGMA cache_size = -65536",  # 64 MiB
    "PRAGMA temp_store = MEMORY",
    "PRAGMA busy_timeout = 5000",
)


@dataclass(slots=True, frozen=True)
class SqliteConfig:
    path: str
    checkpoint_every_s: float = 1.0


class SqlitePostStore:
    """Implements PostStore and Resource."""

    def __init__(self, config: SqliteConfig):
        self._config = config
        self._db = sqlite3.connect(
            config.path, isolation_level=None, check_same_thread=False, cached_statements=32
        )
        self._db.text_factory = bytes  # documents come back as bytes, ready to send
        for pragma in PRAGMAS:
            self._db.execute(pragma)
        self._batch: asyncio.Future | None = None
        self._bound_loop: asyncio.AbstractEventLoop | None = None
        self._timer: asyncio.TimerHandle | None = None

    # --- Resource ---------------------------------------------------------------------------

    async def start(self) -> None:
        self._bound_loop = asyncio.get_running_loop()
        self._schedule_checkpoint()

    async def close(self) -> None:
        if self._timer is not None:
            self._timer.cancel()
            self._timer = None
        if self._batch is not None:
            self._commit()
        self._db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        self._db.close()

    # --- PostStore --------------------------------------------------------------------------

    async def feed(self) -> Document:
        document = self._db.execute(FEED_SQL).fetchone()[0]
        if self._batch is not None:
            await self._batch
        return document

    async def post(self, post_id: int) -> Document | None:
        row = self._db.execute(POST_SQL, (post_id,)).fetchone()
        if self._batch is not None:
            await self._batch
        return None if row is None else row[0]

    async def create_post(self, author: Principal, body: str) -> Document:
        batch = self._begin()
        document = self._db.execute(CREATE_SQL, (author.user_id, body, author.username)).fetchall()[0][0]
        await batch
        return document

    async def like(self, user_id: int, post_id: int) -> LikeOutcome:
        batch = self._begin()
        if self._db.execute(LIKE_SQL, (user_id, post_id)).rowcount == 1:
            outcome = LikeOutcome.LIKED
        elif self._db.execute(EXISTS_SQL, (post_id,)).fetchone() is not None:
            outcome = LikeOutcome.ALREADY_LIKED
        else:
            outcome = LikeOutcome.POST_MISSING
        await batch
        return outcome

    def ping(self) -> None:
        try:
            self._db.execute("SELECT 1").fetchone()
        except sqlite3.Error as exc:
            raise StoreUnavailable(str(exc)) from exc

    # --- group commit -----------------------------------------------------------------------

    @property
    def _loop(self) -> asyncio.AbstractEventLoop:
        return self._bound_loop or asyncio.get_running_loop()

    def _begin(self) -> asyncio.Future:
        """Synchronous, so no other request can run between BEGIN and joining the batch."""
        if self._batch is None:
            self._db.execute("BEGIN IMMEDIATE")
            self._batch = self._loop.create_future()
            self._loop.call_soon(self._commit)
        return self._batch

    def _commit(self) -> None:
        batch, self._batch = self._batch, None
        if batch is None:
            return
        try:
            self._db.execute("COMMIT")
        except sqlite3.Error as exc:
            if self._db.in_transaction:
                self._db.execute("ROLLBACK")
            batch.set_exception(exc)  # every response in the batch becomes a 500
        else:
            batch.set_result(None)

    def _schedule_checkpoint(self) -> None:
        self._timer = self._loop.call_later(self._config.checkpoint_every_s, self._checkpoint)

    def _checkpoint(self) -> None:
        if self._batch is None:
            try:
                self._db.execute("PRAGMA wal_checkpoint(PASSIVE)").fetchall()
            except sqlite3.Error:
                pass  # retried on the next tick
        self._schedule_checkpoint()
