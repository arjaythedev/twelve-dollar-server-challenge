"""Request-thread-owned SQLite connection, ordinary SQL and explicit write commits."""

import sqlite3
from pathlib import Path
from typing import cast

from .models import Post

type PostRow = tuple[int, str, str, str, int]
type CreatedRow = tuple[int, str]


POST_SELECT = """
SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
FROM posts p JOIN users u ON u.id = p.user_id
"""


def _post(row: PostRow) -> Post:
    return Post(id=row[0], body=row[1], created_at=row[2], author=row[3], like_count=row[4])


class Database:
    def __init__(self, path: str) -> None:
        uri = Path(path).resolve().as_uri() + "?mode=rw"
        connection = sqlite3.connect(
            uri, uri=True, isolation_level="DEFERRED", cached_statements=16
        )
        try:
            connection.execute("PRAGMA journal_mode=WAL").fetchall()
            connection.execute("PRAGMA synchronous=NORMAL")
            connection.execute("PRAGMA foreign_keys=ON")
        except sqlite3.Error:
            connection.close()
            raise
        self.connection = connection

    def close(self) -> None:
        self.connection.close()

    def health(self) -> None:
        self.connection.execute("SELECT 1").fetchall()

    def feed(self) -> list[Post]:
        rows = cast(
            list[PostRow],
            self.connection.execute(
                POST_SELECT + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
            ).fetchall(),
        )
        return [_post(row) for row in rows]

    def post(self, post_id: int) -> Post | None:
        row = cast(
            PostRow | None,
            self.connection.execute(POST_SELECT + " WHERE p.id = ?", (post_id,)).fetchone(),
        )
        return _post(row) if row is not None else None

    def create(self, user_id: int, username: str, body: str) -> Post:
        with self.connection:
            rows = cast(
                list[CreatedRow],
                self.connection.execute(
                    "INSERT INTO posts(user_id, body) VALUES (?, ?) RETURNING id, created_at",
                    (user_id, body),
                ).fetchall(),
            )
        # The transaction has committed before this result can reach the response path.
        post_id, created_at = rows[0]
        return Post(id=post_id, body=body, created_at=created_at, author=username, like_count=0)

    def like(self, user_id: int, post_id: int) -> bool | None:
        with self.connection:
            if (
                self.connection.execute("SELECT 1 FROM posts WHERE id = ?", (post_id,)).fetchone()
                is None
            ):
                return None
            cursor = self.connection.execute(
                "INSERT INTO likes(user_id, post_id) VALUES (?, ?) ON CONFLICT(user_id, post_id) DO NOTHING",
                (user_id, post_id),
            )
            created = cursor.rowcount == 1
        return created
