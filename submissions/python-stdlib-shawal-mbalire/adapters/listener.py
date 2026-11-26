"""The listening socket and the accept gate, for a server that accepts on its own (adapters/epoll_http.py).

The listening socket has TCP_DEFER_ACCEPT (Linux), as in PR #84: the kernel wakes us for a new
connection only once its first request has arrived, so the server can accept, read and answer it in
one go. TCP_NODELAY is set on the listener too; Linux copies it to every accepted socket, so
accepting costs no extra setsockopt.

The accept gate keeps descriptors under the limit by counting connections, not by sampling:
asyncio's own accept loop takes up to `backlog` connections per wake-up and, on EMFILE, stops
accepting for a whole second (one 130k run went to 69.6k sockets, hit EMFILE, and the accept queue
overflowed 30,610 times). Here accepting stops while `len(connections)` is within SPARE_DESCRIPTORS
of the limit and starts again as connections close; an EMFILE anyway (descriptors opened elsewhere)
pauses accepting for 50 ms, logged once per pause without a traceback (formatting one opens source
files, which fails too when descriptors are out).
"""

import asyncio
import errno
import logging
import resource
import socket

from adapters.stdlib_http import HttpConnection, HttpServer

# Descriptors kept for everything that is not a client connection: the database and its WAL, the
# listener, the event loop's selector and self-pipe, sockstat, stdio, profiling output.
SPARE_DESCRIPTORS = 64
ACCEPTS_PER_WAKE = 256  # then other ready sockets and timers get their turn
RETRY_AFTER_EMFILE_SECONDS = 0.05
OUT_OF_DESCRIPTORS = (errno.EMFILE, errno.ENFILE)
_log = logging.getLogger(__name__)


def listening_socket(host: str, port: int, backlog: int) -> socket.socket:
    listener = socket.create_server((host, port), backlog=backlog)  # SO_REUSEADDR on POSIX
    listener.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    if hasattr(socket, "TCP_DEFER_ACCEPT"):
        listener.setsockopt(socket.IPPROTO_TCP, socket.TCP_DEFER_ACCEPT, 2)
    listener.setblocking(False)
    return listener


def connection_cap() -> int:
    soft_limit, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
    limit = 65_535 if soft_limit == resource.RLIM_INFINITY else soft_limit
    return max(limit - SPARE_DESCRIPTORS, 1)


class ListeningServer(HttpServer):
    """HttpServer on its own listener and accept gate. A subclass watches the listener for readiness
    (`_watch_listener`) and accepts in `_accept_ready`, asking `_may_accept` before each accept and
    calling `_out_of_descriptors` on EMFILE."""

    __slots__ = ("_listener", "_cap", "_accepting", "_retry")

    def __init__(self, *arguments, **keywords):
        super().__init__(*arguments, **keywords)
        self._listener: socket.socket | None = None
        self._cap = connection_cap()
        self._accepting = False
        self._retry: asyncio.TimerHandle | None = None

    @property
    def sockets(self) -> tuple[socket.socket, ...]:
        return () if self._listener is None else (self._listener,)

    def forget(self, connection: HttpConnection) -> None:
        self.connections.discard(connection)
        if not self._accepting and self._retry is None and self._listener is not None \
                and len(self.connections) < self._cap:
            self._resume_accepting()

    async def _listen(self) -> None:
        self._listener = listening_socket(self._host, self._port, self._backlog)
        self._resume_accepting()

    async def _stop(self) -> None:
        if self._retry is not None:
            self._retry.cancel()
            self._retry = None
        if self._listener is not None:
            if self._accepting:
                self._watch_listener(False)
                self._accepting = False
            self._listener.close()
            self._listener = None
        for connection in list(self.connections):
            connection.close()

    def _may_accept(self) -> bool:
        if len(self.connections) < self._cap:
            return True
        self._watch_listener(False)
        self._accepting = False
        return False

    def _out_of_descriptors(self, error: OSError) -> None:
        _log.warning("out of descriptors (%s) with %d connections; accepting again in %d ms",
                     errno.errorcode.get(error.errno, error.errno), len(self.connections),
                     RETRY_AFTER_EMFILE_SECONDS * 1000)
        self._watch_listener(False)
        self._accepting = False
        self._retry = asyncio.get_running_loop().call_later(RETRY_AFTER_EMFILE_SECONDS, self._retry_accepting)

    def _retry_accepting(self) -> None:
        self._retry = None
        if self._listener is not None:
            self._resume_accepting()

    def _resume_accepting(self) -> None:
        self._accepting = True
        self._watch_listener(True)

    def _watch_listener(self, watch: bool) -> None:
        raise NotImplementedError

    def _accept_ready(self) -> None:
        raise NotImplementedError
