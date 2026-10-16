"""The five API operations. Each request operation is a chain of named Steps run by a Runner.

Every step is one intent (authenticate, decode body, insert post, ...), so a workflow reads as its
step list, and the Runner the composition root injects decides whether the hops are timed
(domain/railway.py). Workflows return Timed[Result[...]]; the spans are empty unless timed.
"""

from collections.abc import Callable

from domain.constants import MAX_ROW_ID
from domain.errors import AppError, ErrorCode, StoreUnavailable
from domain.models import Document, Err, LikeOutcome, LikeReceipt, Ok, Result, Step, Timed
from domain.ports import Clock, PostStore, TokenVerifier
from domain.railway import Runner, run_untimed
from domain.rules import FAIL, bearer_token, checked_post_id, clean_body, like_receipt, principal_from_claims


class FeedApi:
    __slots__ = ("_store", "_tokens", "_clock", "_decode_json", "_started", "_run",
                 "_feed_steps", "_post_steps", "_create_steps", "_like_steps")

    def __init__(
        self,
        store: PostStore,
        tokens: TokenVerifier,
        clock: Clock,
        decode_json: Callable[[bytes], object],  # raises ValueError on malformed input
        run: Runner = run_untimed,
    ):
        self._store = store
        self._tokens = tokens
        self._clock = clock
        self._decode_json = decode_json
        self._started = clock.monotonic()
        self._run = run
        self._feed_steps = (Step("read feed", self._read_feed, awaits=True),)
        self._post_steps = (Step("parse id", checked_post_id), Step("read post", self._read_post, awaits=True))
        self._create_steps = (
            Step("authenticate", self._authenticate),
            Step("decode body", self._decode_body),
            Step("insert post", self._insert_post, awaits=True),
        )
        self._like_steps = (  # auth before the id, per SPEC.md
            Step("authenticate", self._authenticate),
            Step("parse id", self._parse_like_target),
            Step("like post", self._like_post, awaits=True),
        )

    def health(self) -> Result[int]:
        """Seconds since start, once the store answers."""
        try:
            self._store.ping()
        except StoreUnavailable as exc:
            return Err(AppError(ErrorCode.STORE_UNREACHABLE, str(exc)))
        return Ok(int(self._clock.monotonic() - self._started))

    async def feed(self) -> Timed[Result[Document]]:
        return await self._run(None, self._feed_steps)

    async def post(self, raw_id: str) -> Timed[Result[Document]]:
        return await self._run(raw_id, self._post_steps)

    async def create_post(self, authorization: str | None, raw_body: bytes) -> Timed[Result[Document]]:
        return await self._run((authorization, raw_body), self._create_steps)

    async def like(self, authorization: str | None, raw_id: str) -> Timed[Result[LikeReceipt]]:
        return await self._run((authorization, raw_id), self._like_steps)

    # --- steps: one intent each; logic lives in domain/rules.py ------------------------------

    async def _read_feed(self, _: None) -> Result[Document]:
        return Ok(await self._store.feed())

    async def _read_post(self, post_id: int) -> Result[Document]:
        document = await self._store.post(post_id) if post_id <= MAX_ROW_ID else None
        return FAIL[ErrorCode.POST_NOT_FOUND] if document is None else Ok(document)

    def _authenticate(self, request: tuple) -> Result[tuple]:
        """(authorization, rest) -> (caller, rest)."""
        authorization, rest = request
        token = bearer_token(authorization)
        if token is None:
            return FAIL[ErrorCode.MISSING_TOKEN]
        claims = self._tokens.claims(token)
        if claims is None:
            return FAIL[ErrorCode.INVALID_TOKEN]
        caller = principal_from_claims(claims, self._clock.wall())
        return caller if isinstance(caller, Err) else Ok((caller.value, rest))

    def _decode_body(self, request: tuple) -> Result[tuple]:
        """(caller, raw body) -> (caller, clean body)."""
        caller, raw_body = request
        try:
            payload = self._decode_json(raw_body)
        except ValueError:
            return FAIL[ErrorCode.MALFORMED_JSON]
        body = clean_body(payload)
        return body if isinstance(body, Err) else Ok((caller, body.value))

    async def _insert_post(self, request: tuple) -> Result[Document]:
        caller, body = request
        return Ok(await self._store.create_post(caller, body))

    def _parse_like_target(self, request: tuple) -> Result[tuple]:
        """(caller, raw id) -> (caller, post id)."""
        caller, raw_id = request
        post_id = checked_post_id(raw_id)
        return post_id if isinstance(post_id, Err) else Ok((caller, post_id.value))

    async def _like_post(self, request: tuple) -> Result[LikeReceipt]:
        caller, post_id = request
        outcome = (await self._store.like(caller.user_id, post_id)
                   if post_id <= MAX_ROW_ID else LikeOutcome.POST_MISSING)
        return like_receipt(post_id, outcome)
