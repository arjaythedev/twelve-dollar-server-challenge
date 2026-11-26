import json
import tempfile
import unittest
from pathlib import Path

from adapters.span_recorder import SpanRecorder, percentile_ns
from domain.errors import ErrorCode
from domain.models import Ok, Span, Step, Timed
from domain.railway import TimedRunner, run_untimed
from domain.rules import FAIL
from domain.workflows import FeedApi
from tests.fixtures import FakeStore, FakeVerifier, FixedClock, bearer


def add_one(value):
    return Ok(value + 1)


def fail(value):
    return FAIL[ErrorCode.INVALID_POST_ID]


async def double_later(value):
    return Ok(value * 2)


def tagged(name):
    return lambda value: Timed(value + 1, (Span(name, 1),))


class TimedLaws(unittest.TestCase):
    """The Writer monad laws, with spans as the log."""

    def test_left_identity(self):
        self.assertEqual(Timed(1).bind(tagged("a")), tagged("a")(1))

    def test_right_identity(self):
        timed = Timed(1, (Span("a", 5),))
        self.assertEqual(timed.bind(Timed), timed)

    def test_associativity(self):
        timed = Timed(1, (Span("start", 1),))
        left = timed.bind(tagged("a")).bind(tagged("b"))
        right = timed.bind(lambda value: tagged("a")(value).bind(tagged("b")))
        self.assertEqual(left, right)
        self.assertEqual([span.name for span in left.spans], ["start", "a", "b"])

    def test_map_and_tell_keep_spans_in_order(self):
        timed = Timed(1, (Span("a", 1),)).map(str).tell(Span("b", 2))
        self.assertEqual(timed, Timed("1", (Span("a", 1), Span("b", 2))))


class Runners(unittest.IsolatedAsyncioTestCase):
    STEPS = (Step("add", add_one), Step("double", double_later, awaits=True), Step("add again", add_one))

    async def test_untimed_runs_every_step_without_spans(self):
        self.assertEqual(await run_untimed(1, self.STEPS), Timed(Ok(5)))

    async def test_untimed_stops_at_the_first_failure(self):
        steps = (Step("add", add_one), Step("fail", fail), Step("add again", add_one))
        self.assertEqual(await run_untimed(1, steps), Timed(FAIL[ErrorCode.INVALID_POST_ID]))

    async def test_timed_writes_one_span_per_step_run(self):
        timed = await TimedRunner(FixedClock())(1, self.STEPS)
        self.assertEqual(timed, Timed(Ok(5), (Span("add", 1000), Span("double", 1000), Span("add again", 1000))))

    async def test_timed_stops_at_the_first_failure_and_keeps_its_span(self):
        steps = (Step("add", add_one), Step("fail", fail), Step("add again", add_one))
        timed = await TimedRunner(FixedClock())(1, steps)
        self.assertEqual(timed.value, FAIL[ErrorCode.INVALID_POST_ID])
        self.assertEqual([span.name for span in timed.spans], ["add", "fail"])

    async def test_both_runners_give_a_workflow_the_same_result(self):
        token = bearer({"sub": "1", "username": "alice"})
        results = []
        for runner in (run_untimed, TimedRunner(FixedClock())):
            api = FeedApi(FakeStore({5: b"{}"}), FakeVerifier(), FixedClock(), decode_json=json.loads, run=runner)
            results.append((await api.like(token, "5")).value)
        self.assertEqual(results[0], results[1])

    async def test_workflow_hops_are_named(self):
        api = FeedApi(FakeStore(), FakeVerifier(), FixedClock(), decode_json=json.loads, run=TimedRunner(FixedClock()))
        timed = await api.create_post(bearer({"sub": "1", "username": "alice"}), b'{"body":"hi"}')
        self.assertEqual([span.name for span in timed.spans], ["authenticate", "decode body", "insert post"])


class Recorder(unittest.IsolatedAsyncioTestCase):
    def test_percentile_is_the_bucket_upper_bound(self):
        histogram = [0] * 8
        histogram[3] = 99  # 4-7 ns
        histogram[6] = 1  # 32-63 ns
        self.assertEqual(percentile_ns(histogram, 100, 0.5), 8)
        self.assertEqual(percentile_ns(histogram, 100, 1.0), 64)

    async def test_aggregates_and_writes_json_on_close(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "hops.json"
            recorder = SpanRecorder(str(path), flush_every_s=3600)
            await recorder.start()
            recorder.record("GET /feed", (Span("route", 1000), Span("read feed", 20000)))
            recorder.record("GET /feed", (Span("route", 3000), Span("read feed", 22000)))
            await recorder.close()
            written = json.loads(path.read_text())
        self.assertEqual(list(written["GET /feed"]), ["route", "read feed"])
        self.assertEqual(written["GET /feed"]["route"]["count"], 2)
        self.assertEqual(written["GET /feed"]["route"]["mean_us"], 2.0)
        self.assertEqual(written["GET /feed"]["read feed"]["max_us"], 22.0)


if __name__ == "__main__":
    unittest.main()
