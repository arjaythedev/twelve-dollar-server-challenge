"""Domain values and the Result type workflows return."""

from dataclasses import dataclass
from enum import Enum
from typing import Generic, TypeVar

from domain.errors import AppError

T = TypeVar("T")

# A finished JSON document, rendered by the store (SQLite's json_object) and sent as-is.
# The domain treats it as opaque bytes: it never parses or builds response JSON.
Document = bytes


@dataclass(slots=True, frozen=True)
class Principal:
    """The caller, as proven by a verified token. `username` is used as-is, never looked up."""

    user_id: int
    username: str


class LikeOutcome(Enum):
    LIKED = "liked"
    ALREADY_LIKED = "already_liked"
    POST_MISSING = "post_missing"


@dataclass(slots=True, frozen=True)
class LikeReceipt:
    post_id: int
    already_liked: bool


@dataclass(slots=True, frozen=True)
class Ok(Generic[T]):
    value: T


@dataclass(slots=True, frozen=True)
class Err:
    error: AppError


Result = Ok[T] | Err
