"""What the domain needs from the outside world. Protocols cost nothing at runtime."""

from typing import Protocol

from domain.models import Document, LikeOutcome, Principal


class PostStore(Protocol):
    """Posts, likes and their JSON documents.

    Postconditions: every read reflects only committed data, and every write method returns
    only after its change is durable (committed). Raises StoreUnavailable from `ping` when the
    database cannot answer.
    """

    async def feed(self) -> Document: ...  # {"posts":[...20 newest...]}
    async def post(self, post_id: int) -> Document | None: ...  # {"post":{...}}, or None
    async def create_post(self, author: Principal, body: str) -> Document: ...  # {"post":{...}}
    async def like(self, user_id: int, post_id: int) -> LikeOutcome: ...
    def ping(self) -> None: ...


class TokenVerifier(Protocol):
    def claims(self, token: str) -> dict | None:
        """The token's payload if it is well formed, HS256 and correctly signed, else None.
        Expiry and claim shapes are domain rules and are not checked here."""
        ...


class Clock(Protocol):
    """TimePort: wall time for token expiry, monotonic time for durations."""

    def wall(self) -> float: ...
    def monotonic(self) -> float: ...


class Resource(Protocol):
    """LifetimePort: anything holding a resource is started before serving and closed on exit."""

    async def start(self) -> None: ...
    async def close(self) -> None: ...
