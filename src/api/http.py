"""The small HTTP contract consumed from Granian and implemented by test doubles."""

from typing import Protocol


class Headers(Protocol):
    def get(self, key: str, default: str, /) -> str: ...


class Request(Protocol):
    @property
    def method(self) -> str: ...

    @property
    def path(self) -> str: ...

    @property
    def headers(self) -> Headers: ...


class ResponseProtocol(Protocol):
    async def __call__(self) -> bytes: ...

    def response_bytes(self, status: int, headers: list[tuple[str, str]], body: bytes) -> None: ...
