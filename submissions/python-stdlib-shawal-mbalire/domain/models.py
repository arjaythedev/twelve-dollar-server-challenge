"""Domain values, the Result type workflows return, and the Timed monad that carries their spans."""

from collections.abc import Callable
from dataclasses import dataclass
from enum import Enum
from typing import Any

from domain.errors import AppError

# A finished JSON document, rendered by the store (SQLite's json_object) and sent as-is.
# The domain treats it as opaque bytes: it never parses or builds response JSON.
type Document = bytes


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
class Ok[T]:
    value: T


@dataclass(slots=True, frozen=True)
class Err:
    error: AppError


type Result[T] = Ok[T] | Err


@dataclass(slots=True, frozen=True)
class Span:
    """How long one named hop of a request took, in nanoseconds of elapsed time."""

    name: str
    nanoseconds: int


@dataclass(slots=True, frozen=True)
class Timed[T]:
    """The Writer monad over spans: a value plus the spans written while producing it.

    `bind` runs the next computation on the value and concatenates the spans, so a request's
    hops accumulate in order without any shared state. With profiling off, spans stay ().
    """

    value: T
    spans: tuple[Span, ...] = ()

    def bind[U](self, step: Callable[[T], Timed[U]]) -> Timed[U]:
        following = step(self.value)
        return Timed(following.value, self.spans + following.spans)

    def map[U](self, transform: Callable[[T], U]) -> Timed[U]:
        return Timed(transform(self.value), self.spans)

    def tell(self, *spans: Span) -> Timed[T]:
        """The same value with more spans written after the existing ones."""
        return Timed(self.value, self.spans + spans)


@dataclass(slots=True, frozen=True)
class Step:
    """One named hop of a workflow: the previous success value in, a Result out.

    With `awaits`, `run` returns an awaitable of the Result (a store call). The name becomes the
    hop's Span when the workflow runs timed (see domain/railway.py).
    """

    name: str
    run: Callable[[Any], Any]
    awaits: bool = False
