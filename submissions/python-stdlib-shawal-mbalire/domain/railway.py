"""The railway's runners: a workflow's Steps, run along the Result track, returning Timed.

A workflow is a chain of named Steps. A runner passes each step the previous step's success value,
stops at the first Err, and returns the final Result inside Timed. Two interpreters of the same
chain, picked by the composition root:

- run_untimed (Identity): no clock reads, no spans. The production path.
- TimedRunner (Writer): reads the Clock port around every step and writes one Span per step run.

So per-hop profiling measures the real request path, not a copy of it. A Span is elapsed time: for an
awaiting step it includes time spent suspended (e.g. a write waiting for its group commit).
"""

from collections.abc import Sequence
from typing import Any, Protocol

from domain.models import Err, Result, Span, Step, Timed
from domain.ports import Clock


class Runner(Protocol):
    async def __call__(self, start: Any, steps: Sequence[Step]) -> Timed[Result[Any]]: ...


async def run_untimed(start: Any, steps: Sequence[Step]) -> Timed[Result[Any]]:
    """Precondition: `steps` is not empty."""
    value = start
    for step in steps:
        result = step.run(value)
        if step.awaits:
            result = await result
        if isinstance(result, Err):
            return Timed(result)
        value = result.value
    return Timed(result)


class TimedRunner:
    """Runner that writes a Span per step run, timed through the Clock port."""

    __slots__ = ("_clock",)

    def __init__(self, clock: Clock):
        self._clock = clock

    async def __call__(self, start: Any, steps: Sequence[Step]) -> Timed[Result[Any]]:
        """Precondition: `steps` is not empty."""
        perf_ns = self._clock.perf_ns
        spans = []
        value = start
        for step in steps:
            began = perf_ns()
            result = step.run(value)
            if step.awaits:
                result = await result
            spans.append(Span(step.name, perf_ns() - began))
            if isinstance(result, Err):
                break
            value = result.value
        return Timed(result, tuple(spans))
