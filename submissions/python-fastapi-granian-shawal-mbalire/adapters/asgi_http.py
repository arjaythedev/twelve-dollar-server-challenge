"""HTTP driving adapter: a FastAPI app whose routes are raw ASGI endpoints.

FastAPI's dependency solving, Request/Response objects and middleware stack are skipped
(app.middleware_stack = app.router), which PR #10 measured at -36% CPU per request.
Handlers only join byte strings: every success body is either a document SQLite rendered or a
constant template.
"""

from collections.abc import Awaitable, Callable, Sequence
from contextlib import asynccontextmanager

import orjson
from fastapi import FastAPI
from starlette.routing import Route

from adapters.descriptor_budget import DescriptorBudget
from domain.errors import AppError, ErrorCode
from domain.models import Err, Ok, Result
from domain.ports import Resource
from domain.workflows import FeedApi

Scope = dict
Receive = Callable[[], Awaitable[dict]]
Send = Callable[[dict], Awaitable[None]]
Handler = Callable[[Scope, Receive], Awaitable[tuple[int, bytes]]]

_ERRORS: dict[ErrorCode, tuple[int, bytes]] = {
    ErrorCode.INVALID_POST_ID: (400, b'{"error":"invalid post id"}'),
    ErrorCode.POST_NOT_FOUND: (404, b'{"error":"post not found"}'),
    ErrorCode.MISSING_TOKEN: (401, b'{"error":"missing bearer token"}'),
    ErrorCode.INVALID_TOKEN: (401, b'{"error":"invalid or expired token"}'),
    ErrorCode.INVALID_TOKEN_PAYLOAD: (401, b'{"error":"invalid token payload"}'),
    ErrorCode.MALFORMED_JSON: (400, b'{"error":"malformed JSON body"}'),
    ErrorCode.BODY_REQUIRED: (400, b'{"error":"body is required"}'),
    ErrorCode.BODY_TOO_LONG: (400, b'{"error":"body must be at most 500 characters"}'),
}
NOT_FOUND = (404, b'{"error":"not found"}')
INTERNAL_ERROR = (500, b'{"error":"internal server error"}')
HEALTH_OK = b'{"status":"ok","db":"ok","uptime_s":%d}'
LIKED = b'{"liked":true,"already_liked":%s,"post_id":%d}'


def render_error(error: AppError) -> tuple[int, bytes]:
    if error.code is ErrorCode.STORE_UNREACHABLE:
        return 503, orjson.dumps({"status": "degraded", "db": "unreachable", "error": error.detail})
    return _ERRORS[error.code]


def _authorization(scope: Scope) -> str | None:
    for name, value in scope["headers"]:
        if name == b"authorization":
            return value.decode("latin-1")
    return None


async def _read_body(receive: Receive) -> bytes:
    message = await receive()
    body = message.get("body", b"")
    while message.get("more_body", False):
        message = await receive()
        body += message.get("body", b"")
    return body


CONTENT_TYPE = (b"content-type", b"application/json")
CLOSE = (b"connection", b"close")


async def _send(send: Send, status: int, body: bytes, close: bool = False) -> None:
    headers = [CONTENT_TYPE, (b"content-length", b"%d" % len(body))]
    if close:
        headers.append(CLOSE)
    await send({"type": "http.response.start", "status": status, "headers": headers})
    await send({"type": "http.response.body", "body": body})


class AsgiEndpoint:
    """Turns a handler into an ASGI app. Starlette's Route mounts non-function endpoints as-is."""

    __slots__ = ("_handler", "_budget")

    def __init__(self, handler: Handler, budget: DescriptorBudget):
        self._handler = handler
        self._budget = budget

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        try:
            status, body = await self._handler(scope, receive)
        except Exception:
            status, body = INTERNAL_ERROR
        await _send(send, status, body, self._budget.strained)


async def not_found(scope: Scope, receive: Receive, send: Send) -> None:
    if scope["type"] == "http":
        await _send(send, *NOT_FOUND)


def _document(status: int, result: Result[bytes]) -> tuple[int, bytes]:
    match result:
        case Ok(document):
            return status, document
        case Err(error):
            return render_error(error)


def build_routes(api: FeedApi, budget: DescriptorBudget) -> list[Route]:
    async def health(scope: Scope, receive: Receive) -> tuple[int, bytes]:
        match api.health():
            case Ok(uptime_s):
                return 200, HEALTH_OK % uptime_s
            case Err(error):
                return render_error(error)

    async def feed(scope: Scope, receive: Receive) -> tuple[int, bytes]:
        return _document(200, await api.feed())

    async def post(scope: Scope, receive: Receive) -> tuple[int, bytes]:
        return _document(200, await api.post(scope["path_params"]["post_id"]))

    async def create_post(scope: Scope, receive: Receive) -> tuple[int, bytes]:
        raw_body = await _read_body(receive)
        return _document(201, await api.create_post(_authorization(scope), raw_body))

    async def like(scope: Scope, receive: Receive) -> tuple[int, bytes]:
        match await api.like(_authorization(scope), scope["path_params"]["post_id"]):
            case Ok(receipt) if receipt.already_liked:
                return 200, LIKED % (b"true", receipt.post_id)
            case Ok(receipt):
                return 201, LIKED % (b"false", receipt.post_id)
            case Err(error):
                return render_error(error)

    # Routes are matched in order, so the load mix's most frequent requests come first.
    return [
        Route("/feed", AsgiEndpoint(feed, budget), methods=["GET"]),
        Route("/posts/{post_id}", AsgiEndpoint(post, budget), methods=["GET"]),
        Route("/posts/{post_id}/like", AsgiEndpoint(like, budget), methods=["POST"]),
        Route("/posts", AsgiEndpoint(create_post, budget), methods=["POST"]),
        Route("/health", AsgiEndpoint(health, budget), methods=["GET"]),
    ]


def create_app(api: FeedApi, budget: DescriptorBudget, resources: Sequence[Resource]) -> FastAPI:
    @asynccontextmanager
    async def lifespan(app: FastAPI):
        started: list[Resource] = []
        try:
            for resource in resources:
                await resource.start()
                started.append(resource)
            yield
        finally:
            for resource in reversed(started):
                await resource.close()

    app = FastAPI(
        routes=build_routes(api, budget),
        lifespan=lifespan,
        redirect_slashes=False,
        docs_url=None,
        redoc_url=None,
        openapi_url=None,
        # FastAPI's native OpenTelemetry check reads os.environ three times per request.
        telemetry={"tracing": False, "metrics": False, "logs": False},
    )
    app.router.default = not_found
    app.middleware_stack = app.router  # no middleware: requests go straight to routing
    return app
