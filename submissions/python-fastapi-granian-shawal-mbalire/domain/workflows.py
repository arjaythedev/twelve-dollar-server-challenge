"""The five API operations, as pure orchestrators: rules and port calls, no inline logic."""

from collections.abc import Callable

from domain.constants import MAX_ROW_ID
from domain.errors import AppError, ErrorCode, StoreUnavailable
from domain.models import Document, Err, LikeOutcome, LikeReceipt, Ok, Principal, Result
from domain.ports import Clock, PostStore, TokenVerifier
from domain.rules import FAIL, bearer_token, clean_body, parse_post_id, principal_from_claims


class FeedApi:
    __slots__ = ("_store", "_tokens", "_clock", "_decode_json", "_started")

    def __init__(
        self,
        store: PostStore,
        tokens: TokenVerifier,
        clock: Clock,
        decode_json: Callable[[bytes], object],  # raises ValueError on malformed input
    ):
        self._store = store
        self._tokens = tokens
        self._clock = clock
        self._decode_json = decode_json
        self._started = clock.monotonic()

    def health(self) -> Result[int]:
        """Seconds since start, once the store answers."""
        try:
            self._store.ping()
        except StoreUnavailable as exc:
            return Err(AppError(ErrorCode.STORE_UNREACHABLE, str(exc)))
        return Ok(int(self._clock.monotonic() - self._started))

    async def feed(self) -> Result[Document]:
        return Ok(await self._store.feed())

    async def post(self, raw_id: str) -> Result[Document]:
        post_id = parse_post_id(raw_id)
        if post_id is None:
            return FAIL[ErrorCode.INVALID_POST_ID]
        document = await self._store.post(post_id) if post_id <= MAX_ROW_ID else None
        if document is None:
            return FAIL[ErrorCode.POST_NOT_FOUND]
        return Ok(document)

    async def create_post(self, authorization: str | None, raw_body: bytes) -> Result[Document]:
        caller = self._authenticate(authorization)
        if isinstance(caller, Err):
            return caller
        body = self._read_body(raw_body)
        if isinstance(body, Err):
            return body
        return Ok(await self._store.create_post(caller.value, body.value))

    async def like(self, authorization: str | None, raw_id: str) -> Result[LikeReceipt]:
        caller = self._authenticate(authorization)  # auth before the id, per SPEC.md
        if isinstance(caller, Err):
            return caller
        post_id = parse_post_id(raw_id)
        if post_id is None:
            return FAIL[ErrorCode.INVALID_POST_ID]
        outcome = (
            await self._store.like(caller.value.user_id, post_id)
            if post_id <= MAX_ROW_ID
            else LikeOutcome.POST_MISSING
        )
        if outcome is LikeOutcome.POST_MISSING:
            return FAIL[ErrorCode.POST_NOT_FOUND]
        return Ok(LikeReceipt(post_id, already_liked=outcome is LikeOutcome.ALREADY_LIKED))

    def _authenticate(self, authorization: str | None) -> Result[Principal]:
        token = bearer_token(authorization)
        if token is None:
            return FAIL[ErrorCode.MISSING_TOKEN]
        claims = self._tokens.claims(token)
        if claims is None:
            return FAIL[ErrorCode.INVALID_TOKEN]
        return principal_from_claims(claims, self._clock.wall())

    def _read_body(self, raw_body: bytes) -> Result[str]:
        try:
            payload = self._decode_json(raw_body)
        except ValueError:
            return FAIL[ErrorCode.MALFORMED_JSON]
        return clean_body(payload)
