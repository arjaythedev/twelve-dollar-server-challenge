"""Expected failures (values on the failure track) and the one defect type a store raises."""

from dataclasses import dataclass
from enum import Enum


class ErrorCode(Enum):
    INVALID_POST_ID = "invalid_post_id"
    POST_NOT_FOUND = "post_not_found"
    MISSING_TOKEN = "missing_token"
    INVALID_TOKEN = "invalid_token"
    INVALID_TOKEN_PAYLOAD = "invalid_token_payload"
    MALFORMED_JSON = "malformed_json"
    BODY_REQUIRED = "body_required"
    BODY_TOO_LONG = "body_too_long"
    STORE_UNREACHABLE = "store_unreachable"


@dataclass(slots=True, frozen=True)
class AppError:
    code: ErrorCode
    detail: str = ""


class StoreUnavailable(Exception):
    """A store adapter raises this when its backing database cannot answer at all."""
