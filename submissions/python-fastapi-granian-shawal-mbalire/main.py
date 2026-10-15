"""Composition root: settings -> adapters -> FeedApi -> ASGI app. Granian serves `main:app`."""

import gc
import os

import orjson

from adapters.asgi_http import create_app
from adapters.clock import SystemClock
from adapters.cprofile_profiler import CProfileRecorder
from adapters.descriptor_budget import DescriptorBudget
from adapters.jwt_hs256 import Hs256Verifier
from adapters.span_recorder import SpanRecorder
from adapters.sqlite_posts import SqliteConfig, SqlitePostStore
from domain.railway import TimedRunner, run_untimed
from domain.workflows import FeedApi
from infra.config import load_settings

settings = load_settings(os.environ)
clock = SystemClock()
# Hop profiling (PROFILE_HOPS_OUT): the same workflows, run by the timed interpreter, spans recorded.
spans = SpanRecorder(settings.profile_hops_out) if settings.profile_hops_out else None
store = SqlitePostStore(SqliteConfig(path=settings.sqlite_path), spans, clock.perf_ns if spans else None)
runner = TimedRunner(clock) if spans else run_untimed
api = FeedApi(store, Hs256Verifier(settings.jwt_secret), clock, decode_json=orjson.loads, run=runner)
budget = DescriptorBudget()
resources = [store, budget]
resources += [CProfileRecorder(settings.profile_out)] if settings.profile_out else []
resources += [spans] if spans else []
app = create_app(api, budget, resources, spans=spans, clock=clock)

# Everything allocated so far lives forever: move it out of the GC's view and collect rarely.
gc.collect()
gc.freeze()
gc.set_threshold(50_000, 20, 20)
