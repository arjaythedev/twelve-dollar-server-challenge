"""Pure functions: same input, same output, no I/O."""

from domain.constants import BEARER_PREFIX, JS_WHITESPACE, MAX_BODY_CHARS, MAX_ROW_ID
from domain.errors import AppError, ErrorCode
from domain.models import Err, Ok, Principal, Result

# Failures carry no per-request data, so each is built once and shared.
FAIL = {code: Err(AppError(code)) for code in ErrorCode}


def parse_post_id(raw: str) -> int | None:
    """A positive integer in plain decimal digits, else None."""
    if not (raw.isascii() and raw.isdigit()):
        return None
    n = int(raw)
    return n if n > 0 else None


def bearer_token(authorization: str | None) -> str | None:
    if authorization is None or not authorization.startswith(BEARER_PREFIX):
        return None
    return authorization[len(BEARER_PREFIX) :]


def _is_number(value) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def principal_from_claims(claims: dict, now: float) -> Result[Principal]:
    """Checks `exp`/`nbf` against `now`, then the shape of `sub` and `username`."""
    exp = claims.get("exp")
    if exp is not None and (not _is_number(exp) or now >= exp):
        return FAIL[ErrorCode.INVALID_TOKEN]
    nbf = claims.get("nbf")
    if nbf is not None and (not _is_number(nbf) or now < nbf):
        return FAIL[ErrorCode.INVALID_TOKEN]
    sub = claims.get("sub")
    username = claims.get("username")
    user_id = parse_post_id(sub) if isinstance(sub, str) else None
    if user_id is None or user_id > MAX_ROW_ID or not isinstance(username, str):
        return FAIL[ErrorCode.INVALID_TOKEN_PAYLOAD]
    return Ok(Principal(user_id, username))


def clean_body(payload) -> Result[str]:
    """The trimmed `body` field of a decoded request, or why it is unusable."""
    body = payload.get("body") if isinstance(payload, dict) else None
    if not isinstance(body, str):
        return FAIL[ErrorCode.BODY_REQUIRED]
    body = body.strip(JS_WHITESPACE)
    if not body:
        return FAIL[ErrorCode.BODY_REQUIRED]
    if len(body) > MAX_BODY_CHARS:
        return FAIL[ErrorCode.BODY_TOO_LONG]
    return Ok(body)
