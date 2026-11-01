"""Wire contracts in the API's exact JSON key order."""

from typing import Literal, TypedDict


class Post(TypedDict):
    id: int
    body: str
    created_at: str
    author: str
    like_count: int


class HealthResponse(TypedDict):
    status: Literal["ok"]
    db: Literal["ok"]
    uptime_s: int


class DegradedResponse(TypedDict):
    status: Literal["degraded"]
    db: Literal["unreachable"]
    error: str


class PostResponse(TypedDict):
    post: Post


class LikeResponse(TypedDict):
    liked: Literal[True]
    already_liked: bool
    post_id: int


class ErrorResponse(TypedDict):
    error: str


type JSONResponse = HealthResponse | DegradedResponse | PostResponse | LikeResponse | ErrorResponse
type Response = JSONResponse | bytes
