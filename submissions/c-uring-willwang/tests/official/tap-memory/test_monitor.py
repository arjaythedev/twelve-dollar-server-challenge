"""Pure checks for the memory budget, coverage gates and persisted failure classification."""

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
from monitor import APP, GUEST, PAGE, PER_SOCKET, budget, private_paths
from run_memory import MemoryFixture, evaluate


def report(path, *, terminal="service_exit", end=3.2, incomplete=False, killed=False):
    times = [0.8, 1.1, 1.2] if incomplete else [0.8, 1.1, 1.8, 2.4, 2.8]
    identity = dict(kind="identity", original_leaf_max=APP)
    samples = [
        dict(
            kind="sample",
            unix_ns=int(t * 1e9),
            interval_ns=100_000_000,
            events={"oom_kill": 0},
            count=10,
            cap=APP,
            current=150,
            untracked_bytes=10 * PER_SOCKET,
            count_change=0,
            changed=i == 0,
            write_ns=100,
            cycle_ns=1000,
            manager_cpu_ns=i * 1000,
        )
        for i, t in enumerate(times)
    ]
    terminal_row = dict(
        kind="end", reason=terminal, unix_ns=int(end * 1e9), events={"oom_kill": int(killed)}
    )
    (path / "passive-memory.jsonl").write_text(
        "".join(json.dumps(x) + "\n" for x in [identity, *samples, terminal_row])
    )
    (path / "timing.json").write_text(json.dumps(dict(stock_start_unix=1.0, cutoff_unix=3.0)))


class BudgetTests(unittest.TestCase):
    def test_budget(self):
        self.assertEqual(budget(APP, GUEST, 0), APP)
        self.assertEqual(budget(APP, GUEST, 350000), 1230946304)
        self.assertEqual(budget(APP, GUEST, 400000), 1112145920)
        self.assertEqual(budget(APP, GUEST, 2000000), 0)
        for count in (95800, 120000, 349485):
            charged = budget(APP, GUEST, count) + count * PER_SOCKET
            self.assertLessEqual(charged, GUEST)
            self.assertLess(GUEST - charged, PAGE)

    def test_unique_single_scope(self):
        self.assertEqual(
            private_paths("tr0123abc"), ("twelvepassivetr0123abc.slice", "twelve-revisit-tr0123abc.service")
        )
        for bad in ("..", "/system.slice", "tr1234567/../../", "TR1234567"):
            with self.assertRaises(ValueError):
                private_paths(bad)

    def test_score_and_exit(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            report(p)
            self.assertFalse(evaluate(p)["fixture_flags"])
            report(p, terminal="driver_closed", end=2.9)
            self.assertTrue(evaluate(p)["fixture_flags"])
            report(p, incomplete=True)
            self.assertTrue(evaluate(p)["fixture_flags"])
            report(p, incomplete=True, end=2.15, killed=True)
            result = evaluate(p)
            self.assertFalse(result["fixture_flags"])
            self.assertTrue(result["app_exited_before_cutoff"])
            self.assertEqual(result["oom_kill_at_cutoff"], 1)
            # An OOM after the organizer's cutoff does not become a failed scored sample.
            report(p, end=3.12, killed=True)
            self.assertEqual(evaluate(p)["oom_kill_at_cutoff"], 0)

    def test_unaudited_manager_failure_invalidates_an_already_written_score(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / "summary.json").write_text('{"qualified":true,"eligible":true}')
            owner = MemoryFixture(Path("/fixture/relay"))
            owner.out = p
            owner.manager = Mock()
            owner.manager.close.side_effect = RuntimeError("test manager failure")
            with self.assertRaises(RuntimeError):
                owner.finish_memory()
            score = json.loads((p / "summary.json").read_text())
            self.assertFalse(score["qualified"])
            self.assertFalse(score["eligible"])
            self.assertEqual(score["passive_tcp_memory_status"], "fixture-failure")


if __name__ == "__main__":
    unittest.main()
