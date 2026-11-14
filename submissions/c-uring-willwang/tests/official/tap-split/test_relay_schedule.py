from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from relay_schedule import split_worker_schedule


class RelayScheduleTests(unittest.TestCase):
    def samples(self, waits):
        worker = {"tid": 17, "cpu": 8, "role": "client-ingress"}
        result = []
        waited = 0
        for i, delta in enumerate(waits):
            waited += delta
            counters = {"runqueue_wait_ns": waited * 10**9,
                        "run_ns": (i * 10 - waited) * 10**9, "timeslices": i * 3}
            result.append({"unix": i * 10, "relay_worker_scheduling": {"17": counters}})
        return worker, result

    def test_contended_then_recovers(self):
        worker, samples = self.samples([0, 5, 5, 5, 0, 0, 0, 0])
        info, reasons, complete = split_worker_schedule(samples, [worker], 0, 40, 80)
        self.assertTrue(complete)
        self.assertEqual(info["ramp"]["8"]["running_pct"], 50)
        self.assertEqual(info["ramp"]["8"]["runnable_wait_pct"], 50)
        self.assertEqual(info["hold"]["8"]["running_pct"], 100)
        self.assertEqual(len(reasons), 1)
        self.assertIn("ramp", reasons[0])

    def test_short_runnable_delay(self):
        worker, samples = self.samples([0, 6, 0, 0, 0, 0, 0, 0])
        info, reasons, complete = split_worker_schedule(samples, [worker], 0, 40, 80)
        self.assertTrue(complete)
        self.assertEqual(info['ramp']['8']['consecutive_20pct_wait_s'], 10)
        self.assertEqual(reasons, [])

    def test_missing_scheduler_sample(self):
        worker, samples = self.samples([0, 5, 5, 5, 0, 0, 0, 0])
        del samples[1]["relay_worker_scheduling"]["17"]
        info, reasons, complete = split_worker_schedule(samples, [worker], 0, 40, 80)
        self.assertFalse(complete)
        self.assertEqual(info["ramp"]["8"]["coverage_s"], 10)
        self.assertFalse(reasons)


if __name__ == "__main__":
    unittest.main()
