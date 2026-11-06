"""SpanSink and Resource: aggregates request hop spans in memory and writes them as JSON.

Per (endpoint, hop): count, total and maximum nanoseconds, and a power-of-two histogram for
approximate percentiles (each bucket reported at its upper bound). Written every `flush_every_s`
and on close, atomically (temporary file + rename), so a reader never sees half a file. Only
`.name` and `.nanoseconds` of each span are read.
"""

import asyncio
import json
import os
from pathlib import Path

BUCKETS = 48  # 2**47 ns is about 39 hours


def percentile_ns(histogram: list, count: int, fraction: float) -> int:
    """Upper bound of the power-of-two bucket holding the `fraction` quantile."""
    rank = fraction * count
    seen = 0
    for bucket, hits in enumerate(histogram):
        seen += hits
        if seen >= rank:
            return 1 << bucket
    return 1 << (len(histogram) - 1)


def summarize(stats: dict) -> dict:
    """{endpoint: {hop: {count, mean_us, p50_us, p99_us, max_us}}}, hops in first-seen order."""
    out: dict = {}
    for (endpoint, hop), (count, total, largest, histogram) in stats.items():
        out.setdefault(endpoint, {})[hop] = {
            "count": count,
            "mean_us": round(total / count / 1000, 3),
            "p50_us": round(percentile_ns(histogram, count, 0.5) / 1000, 3),
            "p99_us": round(percentile_ns(histogram, count, 0.99) / 1000, 3),
            "max_us": round(largest / 1000, 3),
        }
    return out


class SpanRecorder:
    __slots__ = ("_path", "_flush_every_s", "_stats", "_timer")

    def __init__(self, path: str, flush_every_s: float = 5.0):
        self._path = Path(path)
        self._flush_every_s = flush_every_s
        self._stats: dict = {}
        self._timer: asyncio.TimerHandle | None = None

    def record(self, endpoint: str, spans: tuple) -> None:
        stats = self._stats
        for span in spans:
            key = (endpoint, span.name)
            entry = stats.get(key)
            if entry is None:
                entry = stats[key] = [0, 0, 0, [0] * BUCKETS]
            nanoseconds = span.nanoseconds
            entry[0] += 1
            entry[1] += nanoseconds
            if nanoseconds > entry[2]:
                entry[2] = nanoseconds
            entry[3][min(nanoseconds.bit_length(), BUCKETS - 1)] += 1

    def snapshot(self) -> dict:
        return summarize(self._stats)

    async def start(self) -> None:
        self._schedule()

    async def close(self) -> None:
        if self._timer is not None:
            self._timer.cancel()
            self._timer = None
        self._write()

    def _schedule(self) -> None:
        self._timer = asyncio.get_running_loop().call_later(self._flush_every_s, self._flush)

    def _flush(self) -> None:
        self._write()
        self._schedule()

    def _write(self) -> None:
        self._path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self._path.with_name(self._path.name + ".tmp")
        temporary.write_text(json.dumps(self.snapshot(), indent=1) + "\n")
        os.replace(temporary, self._path)
