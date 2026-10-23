"""Composition root: settings -> adapters -> FeedApi -> HTTP server, on asyncio's own event loop.

    python main.py    # env: SQLITE_PATH, JWT_SECRET, HOST, PORT (infra/ops.py start runs this)
"""

import asyncio
import gc
import os
import signal
from dataclasses import dataclass

from adapters.clock import SystemClock
from adapters.cprofile_profiler import CProfileRecorder
from adapters.descriptor_budget import DescriptorBudget
from adapters.epoll_http import EpollHttpServer
from adapters.json_codec import decode_json
from adapters.jwt_hs256 import Hs256Verifier
from adapters.span_recorder import SpanRecorder
from adapters.sqlite_posts import SqliteConfig, SqlitePostStore
from adapters.stdlib_http import Dispatcher, Handlers
from domain.ports import Resource
from domain.railway import TimedRunner, run_untimed
from domain.workflows import FeedApi
from infra.config import Settings, load_settings


@dataclass(slots=True, frozen=True)
class Application:
    dispatcher: Dispatcher  # what bench/journey.py calls in-process
    resources: list[Resource]  # in start order: the server last, once the store is open


def compose(settings: Settings) -> Application:
    clock = SystemClock()
    # Hop profiling (PROFILE_HOPS_OUT): the same workflows, run by the timed interpreter, spans recorded.
    spans = SpanRecorder(settings.profile_hops_out) if settings.profile_hops_out else None
    store = SqlitePostStore(SqliteConfig(path=settings.sqlite_path), spans, clock.perf_ns if spans else None)
    runner = TimedRunner(clock) if spans else run_untimed
    api = FeedApi(store, Hs256Verifier(settings.jwt_secret), clock, decode_json=decode_json, run=runner)
    budget = DescriptorBudget()
    dispatcher = Dispatcher(Handlers(api), spans, clock if spans else None)
    resources: list[Resource] = [store, budget]
    resources += [CProfileRecorder(settings.profile_out)] if settings.profile_out else []
    resources += [spans] if spans else []
    resources.append(EpollHttpServer(dispatcher, budget, settings.host, settings.port))
    return Application(dispatcher, resources)


async def serve(resources: list[Resource]) -> None:
    """Starts every resource, serves until SIGTERM or SIGINT, then closes them in reverse order."""
    loop = asyncio.get_running_loop()
    stop = asyncio.Event()
    for signal_number in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(signal_number, stop.set)
    started: list[Resource] = []
    try:
        for resource in resources:
            await resource.start()
            started.append(resource)
        # Everything allocated so far lives forever: move it out of the GC's view and collect rarely.
        gc.collect()
        gc.freeze()
        gc.set_threshold(50_000, 20, 20)
        await stop.wait()
    finally:
        for resource in reversed(started):
            await resource.close()


def main() -> None:
    asyncio.run(serve(compose(load_settings(os.environ)).resources))


if __name__ == "__main__":
    main()
