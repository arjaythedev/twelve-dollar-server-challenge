"""HTTP driving adapter on the standard library: asyncio's event loop, adapters/http1.py's parser, and
an HttpConnection per client (the asyncio.Protocol interface). No framework, no third-party package.
adapters/epoll_http.py does the socket I/O.

A request goes: data_received -> parse_request -> Dispatcher.dispatch -> the workflow's coroutine,
driven by hand (coroutine.send) instead of wrapped in an asyncio Task. A read with no group commit
open never suspends, so it is answered inside data_received with no Task, no scheduling and no
extra loop pass. A write suspends on its batch's future; the connection then resumes the coroutine
from that future's done callback, and holds any pipelined requests behind it so answers stay in order.

Handlers only join byte strings: every success body is a document SQLite rendered or a constant
template. With a SpanSink wired in (PROFILE_HOPS_OUT), the Dispatcher times each request's hops
(route, the workflow's own steps, adapter glue, request) and records them.
"""

import asyncio
import functools
import json
import logging
import socket
import time
from collections.abc import Awaitable, Callable, Coroutine
from dataclasses import dataclass
from typing import Any

from adapters.descriptor_budget import DescriptorBudget
from adapters.http1 import CONTINUE, MAX_BODY, MAX_HEAD, BadRequest, Incomplete, Request, ResponseHeads, parse_request
from domain.errors import AppError, ErrorCode
from domain.models import Err, Ok, Result, Span, Timed
from domain.ports import Clock, SpanSink
from domain.workflows import FeedApi

type Response = tuple[int, bytes]  # (status, body)
# (authorization, body, post id as it appeared in the path, "" for routes without one) -> the workflow's result
type Handler = Callable[[bytes | None, bytes, str], Awaitable[Timed[Result]]]
# What the connection drives: a coroutine, and what turns its return value into a Response
type Pending = tuple[Coroutine[Any, Any, Any], Callable[[Any], Response]]

_ERRORS: dict[ErrorCode, Response] = {
    ErrorCode.INVALID_POST_ID: (400, b'{"error":"invalid post id"}'),
    ErrorCode.POST_NOT_FOUND: (404, b'{"error":"post not found"}'),
    ErrorCode.MISSING_TOKEN: (401, b'{"error":"missing bearer token"}'),
    ErrorCode.INVALID_TOKEN: (401, b'{"error":"invalid or expired token"}'),
    ErrorCode.INVALID_TOKEN_PAYLOAD: (401, b'{"error":"invalid token payload"}'),
    ErrorCode.MALFORMED_JSON: (400, b'{"error":"malformed JSON body"}'),
    ErrorCode.BODY_REQUIRED: (400, b'{"error":"body is required"}'),
    ErrorCode.BODY_TOO_LONG: (400, b'{"error":"body must be at most 500 characters"}'),
}
NOT_FOUND: Response = (404, b'{"error":"not found"}')
METHOD_NOT_ALLOWED: Response = (405, b'{"error":"method not allowed"}')  # out of scope in SPEC.md
INTERNAL_ERROR: Response = (500, b'{"error":"internal server error"}')
HEALTH_OK = b'{"status":"ok","db":"ok","uptime_s":%d}'
LIKED = b'{"liked":true,"already_liked":%s,"post_id":%d}'
LIKE_SUFFIX = b"/like"
_log = logging.getLogger(__name__)


def internal_error() -> Response:
    """The 500 answer for an unexpected exception, which is logged: a bug or a failed commit."""
    _log.exception("request failed")
    return INTERNAL_ERROR


def render_error(error: AppError) -> Response:
    if error.code is ErrorCode.STORE_UNREACHABLE:
        return 503, json.dumps({"status": "degraded", "db": "unreachable", "error": error.detail},
                               separators=(",", ":"), ensure_ascii=False).encode()
    return _ERRORS[error.code]


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


def _text(header: bytes | None) -> str | None:
    return None if header is None else header.decode("latin-1")


class Handlers:
    """The five endpoints, each (authorization, body, post id) -> the workflow's coroutine.

    Plain functions, not coroutines: each returns the workflow's own coroutine, so a request drives
    one coroutine chain fewer.
    """

    __slots__ = ("_api",)

    def __init__(self, api: FeedApi):
        self._api = api

    async def health(self, authorization: bytes | None, body: bytes, post_id: str) -> Timed[Result]:
        return Timed(self._api.health())

    def feed(self, authorization: bytes | None, body: bytes, post_id: str) -> Awaitable[Timed[Result]]:
        return self._api.feed()

    def post(self, authorization: bytes | None, body: bytes, post_id: str) -> Awaitable[Timed[Result]]:
        return self._api.post(post_id)

    def create_post(self, authorization: bytes | None, body: bytes, post_id: str) -> Awaitable[Timed[Result]]:
        return self._api.create_post(_text(authorization), body)

    def like(self, authorization: bytes | None, body: bytes, post_id: str) -> Awaitable[Timed[Result]]:
        return self._api.like(_text(authorization), post_id)


def _finisher(render: Callable[[Result], Response]) -> Callable[[Timed[Result]], Response]:
    def finish(timed: Timed[Result]) -> Response:
        return render(timed.value)

    return finish


@dataclass(slots=True, frozen=True)
class Route:
    method: bytes
    handler: Handler
    finish: Callable[[Timed[Result]], Response]  # the handler's result -> (status, body)
    endpoint: str  # the name hop spans are recorded under


def _identity(response: Response) -> Response:
    return response


class Dispatcher:
    """Picks the handler by exact path; returns a Response, or a Pending for the connection to drive.

    Matches the FastAPI entry: /posts/{id} and /posts/{id}/like take any non-empty segment without a
    slash, and the workflow validates it. Unknown paths get 404; a known path with the wrong method
    gets 405 (SPEC.md leaves that case open).
    """

    __slots__ = ("_exact", "_post", "_like", "_spans", "_clock")

    def __init__(self, handlers: Handlers, spans: SpanSink | None = None, clock: Clock | None = None):
        def route(method: bytes, handler: Handler, render: Callable[[Result], Response], endpoint: str) -> Route:
            return Route(method, handler, _finisher(render), endpoint)

        self._exact: dict[bytes, Route] = {
            b"/feed": route(b"GET", handlers.feed, _ok, "GET /feed"),
            b"/posts": route(b"POST", handlers.create_post, _created, "POST /posts"),
            b"/health": route(b"GET", handlers.health, _health, "GET /health"),
        }
        self._post = route(b"GET", handlers.post, _ok, "GET /posts/:id")
        self._like = route(b"POST", handlers.like, _liked, "POST /posts/:id/like")
        self._spans = spans
        self._clock = clock
        if spans is not None and clock is None:
            raise ValueError("timing requests needs a clock")

    def route(self, path: bytes) -> tuple[Route | None, str]:
        """(route or None, post id)."""
        if (route := self._exact.get(path)) is not None:
            return route, ""
        if not path.startswith(b"/posts/"):
            return None, ""
        raw_id = path[7:]
        route = self._post
        if raw_id.endswith(LIKE_SUFFIX):
            raw_id = raw_id[:-5]
            route = self._like
        if not raw_id or b"/" in raw_id:
            return None, ""
        return route, raw_id.decode("latin-1")

    def dispatch(self, request: Request) -> Response | Pending:
        if self._spans is not None:
            return self._timed(request), _identity
        route, post_id = self.route(request.path)
        if route is None:
            return NOT_FOUND
        if route.method != request.method:
            return METHOD_NOT_ALLOWED
        return route.handler(request.authorization, request.body, post_id), route.finish

    async def _timed(self, request: Request) -> Response:
        """The same request, with every hop written as a Span and recorded."""
        perf_ns = self._clock.perf_ns
        began = perf_ns()
        route, post_id = self.route(request.path)
        routed = perf_ns()
        timed: Timed = Timed(None)
        if route is None:
            endpoint, response = "not found", NOT_FOUND
        elif route.method != request.method:
            endpoint, response = "wrong method", METHOD_NOT_ALLOWED
        else:
            endpoint = route.endpoint
            try:
                timed = await route.handler(request.authorization, request.body, post_id)
                response = route.finish(timed)
            except Exception:
                response = internal_error()
        handled = perf_ns()
        glue = handled - routed - sum(span.nanoseconds for span in timed.spans)
        timed = timed.tell(Span("adapter glue", glue), Span("request", handled - began))
        self._spans.record(endpoint, (Span("route", routed - began),) + timed.spans)
        return response


class HttpConnection(asyncio.Protocol):
    """One client connection: HTTP/1.1 keep-alive, pipelining, Content-Length or chunked bodies.

    While a response is pending (a write waiting for its commit) or the transport's write buffer is
    full, further bytes are only buffered. `Connection: close` goes out when the client asked for it
    or the descriptor budget is strained; the transport then closes once the response is written.
    """

    __slots__ = ("_server", "_transport", "_buffer", "_waiting", "_paused", "_continued", "active")

    def __init__(self, server: HttpServer):
        self._server = server
        self._transport: asyncio.Transport | None = None
        self._buffer = b""
        self._waiting = False  # a request's coroutine is suspended
        self._paused = False  # the transport asked us to stop writing
        self._continued = False  # 100 Continue was sent for the request in progress
        self.active = True  # had a request since the last idle sweep

    def connection_made(self, transport: asyncio.Transport) -> None:
        self._transport = transport
        self._server.connections.add(self)

    def connection_lost(self, exc: Exception | None) -> None:
        self._transport = None
        self._buffer = b""
        self._server.forget(self)

    def pause_writing(self) -> None:
        self._paused = True

    def resume_writing(self) -> None:
        self._paused = False
        self._continue_buffered()

    def data_received(self, data: bytes) -> None:
        self.active = True
        if self._buffer:
            data = self._buffer + data
            self._buffer = b""
        if self._waiting or self._paused:
            if len(data) > MAX_HEAD + MAX_BODY:  # a client that sends but never reads
                self._transport.abort()
                return
            self._buffer = data
            return
        self._process(data)

    def close(self) -> None:
        if self._transport is not None:
            self._transport.close()

    def close_if_idle(self) -> None:
        """Closes the connection if it had no request since the last call; then starts a new period."""
        if self.active:
            self.active = False
        elif not self._waiting:
            self.close()

    def _process(self, buffer: bytes) -> None:
        """Answers every complete request in `buffer`; keeps the rest for the next read."""
        dispatch = self._server.dispatch
        start = 0
        end = len(buffer)
        while start < end:
            try:
                request = parse_request(buffer, start)
            except BadRequest as bad:
                self._send((bad.status, b'{"error":"%s"}' % bad.message), keep_alive=False)
                return
            if isinstance(request, Incomplete):
                if request.expects_continue and not self._continued:
                    self._continued = True
                    self._transport.write(CONTINUE)
                break
            self._continued = False
            start = request.end
            outcome = dispatch(request)
            if type(outcome) is tuple and type(outcome[0]) is int:
                response = outcome
            else:
                coroutine, finish = outcome
                try:
                    future = coroutine.send(None)
                except StopIteration as done:
                    response = finish(done.value)
                except Exception:
                    response = internal_error()
                else:
                    self._suspend(future, coroutine, finish, request.keep_alive)
                    if start < end:
                        self._buffer = buffer[start:]
                    return
            if not self._send(response, request.keep_alive):
                return
            if self._paused:
                break
        if start < end:
            self._buffer = buffer[start:]

    def _suspend(self, future: Any, coroutine: Coroutine, finish: Callable[[Any], Response], keep_alive: bool) -> None:
        self._waiting = True
        resume = functools.partial(self._resume, coroutine, finish, keep_alive)
        if hasattr(future, "add_done_callback"):  # an asyncio future: our store's group commit
            # What asyncio.Task does with a yielded future: while this flag is set, the next `await`
            # of the same future (another request waiting for the same batch) raises RuntimeError.
            future._asyncio_future_blocking = False
            future.add_done_callback(resume)
        else:  # a bare yield (asyncio.sleep(0)): try again on the next loop pass
            asyncio.get_running_loop().call_soon(resume, None)

    def _resume(self, coroutine: Coroutine, finish: Callable[[Any], Response], keep_alive: bool, _: Any) -> None:
        try:
            future = coroutine.send(None)
        except StopIteration as done:
            response = finish(done.value)
        except Exception:
            response = internal_error()
        else:
            self._suspend(future, coroutine, finish, keep_alive)
            return
        self._waiting = False
        if self._transport is None:  # the client left while its write committed
            return
        if self._send(response, keep_alive):
            self._continue_buffered()

    def _continue_buffered(self) -> None:
        if self._buffer and not self._waiting and not self._paused and self._transport is not None:
            buffer, self._buffer = self._buffer, b""
            self._process(buffer)

    def _send(self, response: Response, keep_alive: bool) -> bool:
        """Writes `response`; True if the connection stays open."""
        close = not keep_alive or self._server.budget.strained
        status, body = response
        self._transport.write(self._server.heads.frame(status, body, close))
        if close:
            self._transport.close()
            self._buffer = b""
        return not close


class HttpServer:
    """Resource: listens on `host`:`port` and serves the Dispatcher until closed.

    Once a second it refreshes the Date in the response heads; every `idle_seconds` it closes the
    connections that sent nothing since the previous sweep (so an idle one lives 1-2 periods; the
    SPEC asks for at least 65 s).

    The socket I/O is a subclass's (adapters/epoll_http.py): it overrides `_listen`, `_stop`, `sockets`
    and `forget`. (asyncio's own loop.create_server was measured and dropped: see AGENTS.md.)
    """

    __slots__ = ("dispatch", "budget", "heads", "connections", "_host", "_port", "_backlog",
                 "_idle_seconds", "_timers")

    def __init__(self, dispatcher: Dispatcher, budget: DescriptorBudget, host: str, port: int,
                 backlog: int = 4096, idle_seconds: float = 120.0):
        self.dispatch = dispatcher.dispatch
        self.budget = budget
        self.heads = ResponseHeads()
        self.connections: set[HttpConnection] = set()
        self._host = host
        self._port = port
        self._backlog = backlog
        self._idle_seconds = idle_seconds
        self._timers: dict[str, asyncio.TimerHandle] = {}

    @property
    def sockets(self) -> tuple[socket.socket, ...]:
        return ()

    async def start(self) -> None:
        await self._listen()
        self._tick_date()
        self._timers["sweep"] = asyncio.get_running_loop().call_later(self._idle_seconds, self._sweep)

    async def close(self) -> None:
        for timer in self._timers.values():
            timer.cancel()
        await self._stop()

    def forget(self, connection: HttpConnection) -> None:
        """Called by a connection once it is closed."""
        self.connections.discard(connection)

    async def _listen(self) -> None:
        raise NotImplementedError

    async def _stop(self) -> None:
        raise NotImplementedError

    def _tick_date(self) -> None:
        now = time.time()
        self.heads.refresh(now)
        # wake just after the next second starts
        self._timers["date"] = asyncio.get_running_loop().call_later(1.0 - now % 1.0 + 0.001, self._tick_date)

    def _sweep(self) -> None:
        for connection in list(self.connections):
            connection.close_if_idle()
        self._timers["sweep"] = asyncio.get_running_loop().call_later(self._idle_seconds, self._sweep)
