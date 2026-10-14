"""Composition root: settings -> adapters -> FeedApi -> ASGI app. Granian serves `main:app`."""

import gc
import os

import orjson

from adapters.asgi_http import create_app
from adapters.clock import SystemClock
from adapters.cprofile_profiler import CProfileRecorder
from adapters.descriptor_budget import DescriptorBudget
from adapters.jwt_hs256 import Hs256Verifier
from adapters.sqlite_posts import SqliteConfig, SqlitePostStore
from domain.workflows import FeedApi
from infra.config import load_settings

settings = load_settings(os.environ)
store = SqlitePostStore(SqliteConfig(path=settings.sqlite_path))
api = FeedApi(store, Hs256Verifier(settings.jwt_secret), SystemClock(), decode_json=orjson.loads)
budget = DescriptorBudget()
resources = [store, budget] + ([CProfileRecorder(settings.profile_out)] if settings.profile_out else [])
app = create_app(api, budget, resources)

# Everything allocated so far lives forever: move it out of the GC's view and collect rarely.
gc.collect()
gc.freeze()
gc.set_threshold(50_000, 20, 20)
