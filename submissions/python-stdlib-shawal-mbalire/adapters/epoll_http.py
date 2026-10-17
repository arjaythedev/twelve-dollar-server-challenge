"""HTTP server on a hand-written epoll loop, nested in asyncio's.

Option 3 of the handover, PR #84's design on CPython. Chosen over asyncio's create_server and over an
own accept path with asyncio transports by an alternating 130k A/B (AGENTS.md): ~26% and ~18% less
CPU per request, no accept-queue overflows, ~45% less memory. asyncio's selector watches one descriptor:
this server's epoll instance. When it is readable, `_poll` takes the ready sockets from it and
reads, parses and writes them with os.read/os.write: no transport, no protocol callbacks scheduled
per event, no selector key per connection. Timers, the group commit and the WAL checkpoint stay on
asyncio's loop, so the store and the workflows are unchanged.

The listening socket is in the same epoll set. With TCP_DEFER_ACCEPT a new connection wakes us only
once its first request has arrived: it is accepted, read and answered at once, and one that is
retired (descriptor budget strained) is closed again without ever entering the epoll set.

Each connection is still an HttpConnection (adapters/stdlib_http.py), which parses, pipelines and
drives the workflow coroutines; EpollTransport gives it the transport calls it makes. On a Mac
(no epoll) the same loop runs on `selectors` (kqueue), for the tests and local runs.
"""

import asyncio
import os
import select
import selectors

from adapters.listener import ACCEPTS_PER_WAKE, OUT_OF_DESCRIPTORS, ListeningServer
from adapters.stdlib_http import HttpConnection

IN, OUT = 0x001, 0x004  # EPOLLIN, EPOLLOUT
READ_SIZE = 65_536
EVENTS_PER_WAKE = 1024  # more stay ready: asyncio calls _poll again on its next pass
HIGH_WATER = 64 * 1024  # unsent bytes past which the connection is asked to pause writing


class SelectorPoller:
    """epoll's register/modify/unregister/poll over `selectors`, where there is no epoll."""

    __slots__ = ("_selector",)

    def __init__(self):
        self._selector = selectors.DefaultSelector()

    def fileno(self) -> int:
        return self._selector.fileno()

    def register(self, descriptor: int, mask: int) -> None:
        self._selector.register(descriptor, (selectors.EVENT_READ if mask & IN else 0)
                                | (selectors.EVENT_WRITE if mask & OUT else 0))

    def modify(self, descriptor: int, mask: int) -> None:
        self._selector.modify(descriptor, (selectors.EVENT_READ if mask & IN else 0)
                              | (selectors.EVENT_WRITE if mask & OUT else 0))

    def unregister(self, descriptor: int) -> None:
        try:
            self._selector.unregister(descriptor)
        except KeyError:
            pass

    def poll(self, timeout: float, maxevents: int) -> list[tuple[int, int]]:
        return [(key.fd, (IN if events & selectors.EVENT_READ else 0) | (OUT if events & selectors.EVENT_WRITE else 0))
                for key, events in self._selector.select(timeout)[:maxevents]]

    def close(self) -> None:
        self._selector.close()


def _nothing(descriptor: int) -> None:
    pass


class EpollTransport:
    """One accepted socket, written with os.write; unsent bytes wait for EPOLLOUT."""

    __slots__ = ("descriptor", "_server", "_connection", "_pending", "closing", "_mask", "_paused")

    def __init__(self, server: EpollHttpServer, descriptor: int, connection: HttpConnection):
        self.descriptor = descriptor
        self._server = server
        self._connection = connection
        self._pending = b""
        self.closing = False  # close once the pending bytes are out; read nothing more
        self._mask = 0  # 0: not in the epoll set
        self._paused = False  # the connection was asked to pause writing

    def write(self, data: bytes) -> None:
        if self.descriptor < 0:
            return
        if self._pending:
            self._pending += data
        else:
            try:
                sent = os.write(self.descriptor, data)
            except BlockingIOError:
                sent = 0
            except OSError:
                self.abort()
                return
            if sent == len(data):
                return
            self._pending = data[sent:]
            self.watch(IN | OUT)
        if len(self._pending) > HIGH_WATER and not self._paused:
            self._paused = True
            self._connection.pause_writing()

    def flush(self) -> None:
        if self._pending:
            try:
                sent = os.write(self.descriptor, self._pending)
            except BlockingIOError:
                return
            except OSError:
                self.abort()
                return
            self._pending = self._pending[sent:]
            if self._pending:
                return
        if self.closing:
            self._close_now()
            return
        self.watch(IN)
        if self._paused:
            self._paused = False
            self._connection.resume_writing()

    def read(self) -> None:
        try:
            data = os.read(self.descriptor, READ_SIZE)
        except BlockingIOError:
            return
        except OSError:
            self.abort()
            return
        if data:
            self._connection.data_received(data)
        else:  # the client closed its side
            self.close()

    def close(self) -> None:
        if self.descriptor < 0 or self.closing:
            return
        if self._pending:
            self.closing = True
            self.watch(OUT)
        else:
            self._close_now()

    def abort(self) -> None:
        if self.descriptor >= 0:
            self._pending = b""
            self._close_now()

    def watch(self, mask: int) -> None:
        if mask == self._mask:
            return
        if self._mask:
            self._server.poller.modify(self.descriptor, mask)
        else:
            self._server.poller.register(self.descriptor, mask)
        self._mask = mask

    def _close_now(self) -> None:
        descriptor, self.descriptor = self.descriptor, -1
        self._server.transports.pop(descriptor, None)
        if self._mask:
            self._server.unregister(descriptor)  # epoll drops a closed descriptor by itself
        os.close(descriptor)
        self._connection.connection_lost(None)


class EpollHttpServer(ListeningServer):
    __slots__ = ("poller", "unregister", "transports")

    def __init__(self, *arguments, **keywords):
        super().__init__(*arguments, **keywords)
        if hasattr(select, "epoll"):
            self.poller = select.epoll()
            self.unregister = _nothing
        else:
            self.poller = SelectorPoller()
            self.unregister = self.poller.unregister
        self.transports: dict[int, EpollTransport] = {}

    async def _listen(self) -> None:
        asyncio.get_running_loop().add_reader(self.poller.fileno(), self._poll)
        await super()._listen()

    async def _stop(self) -> None:
        await super()._stop()
        for transport in list(self.transports.values()):
            transport.abort()
        asyncio.get_running_loop().remove_reader(self.poller.fileno())
        self.poller.close()

    def _watch_listener(self, watch: bool) -> None:
        if watch:
            self.poller.register(self._listener.fileno(), IN)
        else:
            self.poller.unregister(self._listener.fileno())

    def _poll(self) -> None:
        listener = self._listener.fileno() if self._listener is not None else -1
        transports = self.transports
        for descriptor, events in self.poller.poll(0, EVENTS_PER_WAKE):
            if descriptor == listener:
                self._accept_ready()
                continue
            transport = transports.get(descriptor)
            if transport is None:  # closed earlier in this batch
                continue
            if events & OUT:
                transport.flush()
            if events & ~OUT and transport.descriptor >= 0:
                if not transport.closing:
                    transport.read()  # data, or the error / hang-up that read() then reports
                elif events & ~(IN | OUT):  # hang-up or error while the last bytes wait to go out
                    transport.abort()

    def _accept_ready(self) -> None:
        accept = self._listener._accept  # the descriptor alone: no socket object per connection
        transports = self.transports
        for _ in range(ACCEPTS_PER_WAKE):
            if not self._may_accept():
                return
            try:
                descriptor, _ = accept()
            except BlockingIOError:
                return
            except OSError as error:
                if error.errno in OUT_OF_DESCRIPTORS:
                    self._out_of_descriptors(error)
                    return
                continue  # ECONNABORTED and the like: that client is gone
            os.set_blocking(descriptor, False)
            connection = HttpConnection(self)
            transport = EpollTransport(self, descriptor, connection)
            transports[descriptor] = transport
            connection.connection_made(transport)
            transport.read()  # usually its first request (TCP_DEFER_ACCEPT)
            if transport.descriptor >= 0 and not transport.closing:
                transport.watch(transport._mask | IN)
