"""HTTP driving adapter: a FastAPI app that routes its five endpoints itself.

FastAPI's dependency solving, Request/Response objects and middleware stack are skipped, which PR #10
measured at -36% CPU per request. Starlette's Router is skipped too (app.middleware_stack is a
Dispatcher): its regex matching and scope copying were 5.7% of the server's CPU at 40k users. The
router still runs the lifespan.
Handlers only join byte strings: every success body is either a document SQLite rendered or a
constant template.

Workflows return Timed results (domain/railway.py); each route renders its handler's result to
(status, body) with a pure function (_ok, _created, _liked, _health). With a SpanSink wired in, the Dispatcher adds its own hops (route, adapter glue, send, request) to the
workflow's step spans and records them; without one it reads no clock at all.
"""

from collections.abc import Awaitable, Callable, Sequence
from contextlib import asynccontextmanager

import orjson
from fastapi import FastAPI

from adapters.descriptor_budget import DescriptorBudget
from domain.errors import AppError, ErrorCode
from domain.models import Err, Ok, Result, Span, Timed
from domain.ports import Clock, Resource, SpanSink
from domain.workflows import FeedApi

Scope = dict
Receive = Callable[[], Awaitable[dict]]
Send = Callable[[dict], Awaitable[None]]
ASGIApp = Callable[[Scope, Receive, Send], Awaitable[None]]
Response = tuple[int, bytes]  # (status, body)
# (scope, receive, post id as it appeared in the path, "" for routes without one) -> Timed result
Handler = Callable[[Scope, Receive, str], Awaitable[Timed[Result]]]
Route = tuple[str, Handler, Callable[[Result], Response], str]  # (method, handler, render, endpoint)

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
METHOD_NOT_ALLOWED = (405, b'{"error":"method not allowed"}')  # out of scope in SPEC.md
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


def _document(status: int, result: Result[bytes]) -> Response:
    match result:
        case Ok(document):
            return status, document
        case Err(error):
            return render_error(error)


def _ok(document: Result[bytes]) -> Response:
    return _document(200, document)


def _created(document: Result[bytes]) -> Response:
    return _document(201, document)


def _liked(result: Result) -> Response:
    match result:
        case Ok(receipt) if receipt.already_liked:
            return 200, LIKED % (b"true", receipt.post_id)
        case Ok(receipt):
            return 201, LIKED % (b"false", receipt.post_id)
        case Err(error):
            return render_error(error)


def _health(result: Result[int]) -> Response:
    match result:
        case Ok(uptime_s):
            return 200, HEALTH_OK % uptime_s
        case Err(error):
            return render_error(error)


class Handlers:
    """The five endpoints, each (scope, receive, post_id) -> the workflow's Timed result."""

    __slots__ = ("_api",)

    def __init__(self, api: FeedApi):
        self._api = api

    async def health(self, scope: Scope, receive: Receive, post_id: str) -> Timed[Result]:
        return Timed(self._api.health())

    async def feed(self, scope: Scope, receive: Receive, post_id: str) -> Timed[Result]:
        return await self._api.feed()

    async def post(self, scope: Scope, receive: Receive, post_id: str) -> Timed[Result]:
        return await self._api.post(post_id)

    async def create_post(self, scope: Scope, receive: Receive, post_id: str) -> Timed[Result]:
        raw_body = await _read_body(receive)
        return await self._api.create_post(_authorization(scope), raw_body)

    async def like(self, scope: Scope, receive: Receive, post_id: str) -> Timed[Result]:
        return await self._api.like(_authorization(scope), post_id)


LIKE_SUFFIX = "/like"


class Dispatcher:
    """ASGI app: picks the handler by exact path and sends its answer.

    Matches what the Starlette routes did: /posts/{post_id} and /posts/{post_id}/like take any
    non-empty segment without a slash, and the handler validates it. Unknown paths get 404; a known
    path with the wrong method gets 405 (SPEC.md leaves that case open). Anything that isn't HTTP
    (the lifespan) goes to `other`, Starlette's router.

    With `spans` (and `clock`) set, every request is timed: the hops are route, the workflow's own
    steps, adapter glue (handler time not spent in a step: body read, rendering, coroutines), send,
    and request (all of it). Granian's and the kernel's work before and after isn't visible here.
    """

    __slots__ = ("_exact", "_post", "_like", "_budget", "_other", "_spans", "_clock")

    def __init__(self, handlers: Handlers, budget: DescriptorBudget, other: ASGIApp,
                 spans: SpanSink | None = None, clock: Clock | None = None):
        self._exact: dict[str, Route] = {
            "/feed": ("GET", handlers.feed, _ok, "GET /feed"),
            "/posts": ("POST", handlers.create_post, _created, "POST /posts"),
            "/health": ("GET", handlers.health, _health, "GET /health"),
        }
        self._post: Route = ("GET", handlers.post, _ok, "GET /posts/:id")
        self._like: Route = ("POST", handlers.like, _liked, "POST /posts/:id/like")
        self._budget = budget
        self._other = other
        self._spans = spans
        self._clock = clock
        if spans is not None and clock is None:
            raise ValueError("timing requests needs a clock")

    def route(self, path: str) -> tuple[Route | None, str]:
        """(route or None, post id)."""
        route = self._exact.get(path)
        if route is not None:
            return route, ""
        if not path.startswith("/posts/"):
            return None, ""
        post_id = path[7:]
        route = self._post
        if post_id.endswith(LIKE_SUFFIX):
            post_id = post_id[:-5]
            route = self._like
        if not post_id or "/" in post_id:
            return None, ""
        return route, post_id

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self._other(scope, receive, send)
            return
        if self._spans is not None:
            await self._timed_call(scope, receive, send)
            return
        route, post_id = self.route(scope["path"])
        if route is None:
            status, body = NOT_FOUND
        elif route[0] != scope["method"]:
            status, body = METHOD_NOT_ALLOWED
        else:
            try:
                status, body = route[2]((await route[1](scope, receive, post_id)).value)
            except Exception:
                status, body = INTERNAL_ERROR
        await _send(send, status, body, self._budget.strained)

    async def _timed_call(self, scope: Scope, receive: Receive, send: Send) -> None:
        """The same request, with every hop written as a Span and recorded."""
        perf_ns = self._clock.perf_ns
        began = perf_ns()
        route, post_id = self.route(scope["path"])
        routed = perf_ns()
        timed: Timed = Timed(None)
        if route is None:
            endpoint, (status, body) = "not found", NOT_FOUND
        elif route[0] != scope["method"]:
            endpoint, (status, body) = "wrong method", METHOD_NOT_ALLOWED
        else:
            endpoint = route[3]
            try:
                timed = await route[1](scope, receive, post_id)
                status, body = route[2](timed.value)
            except Exception:
                status, body = INTERNAL_ERROR
        handled = perf_ns()
        await _send(send, status, body, self._budget.strained)
        sent = perf_ns()
        glue = handled - routed - sum(span.nanoseconds for span in timed.spans)
        timed = timed.tell(Span("adapter glue", glue), Span("send", sent - handled), Span("request", sent - began))
        self._spans.record(endpoint, (Span("route", routed - began),) + timed.spans)


def create_app(api: FeedApi, budget: DescriptorBudget, resources: Sequence[Resource],
               spans: SpanSink | None = None, clock: Clock | None = None) -> FastAPI:
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
        lifespan=lifespan,
        redirect_slashes=False,
        docs_url=None,
        redoc_url=None,
        openapi_url=None,
        # FastAPI's native OpenTelemetry check reads os.environ three times per request.
        telemetry={"tracing": False, "metrics": False, "logs": False},
    )
    # no middleware and no Starlette routing: requests go straight to the dispatcher
    app.middleware_stack = Dispatcher(Handlers(api), budget, app.router, spans, clock)
    return app
