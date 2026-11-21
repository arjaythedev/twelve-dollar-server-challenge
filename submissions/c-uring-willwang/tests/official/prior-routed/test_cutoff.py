"""Offline regression tests for the scored cutoff."""

import json
from pathlib import Path
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from assess_window import classify_lifetime, qdisc_window, scheduled_dials_match
from observe import dump


def sample(t, live=True, oom=0, fresh=True):
    return {
        "wall": t,
        "cgroup_live_sample": fresh,
        "server_pid": {"pid": 1} if live else {},
        "server_mem": {"events": {"oom_kill": oom}},
    }


class Cutoff(unittest.TestCase):
    def test_each_shard_dials_matches_its_assigned_users(self):
        occupancy = {
            "expected": [32, 32, 32, 32],
            "raw_users": [32, 32, 32, 32],
            "dials": [32, 32, 32, 32],
        }
        merged = {"dials": 128}
        self.assertTrue(scheduled_dials_match(occupancy, merged, 128))
        occupancy["dials"] = [31, 33, 32, 32]
        self.assertFalse(scheduled_dials_match(occupancy, merged, 128))

    def test_exit_in_hold(self):
        self.assertEqual(
            classify_lifetime(
                [sample(0), sample(99, False), sample(100.01, False)], None, 1, 100
            )["state"],
            "scored_exit",
        )

    def test_scored_oom(self):
        self.assertEqual(
            classify_lifetime(
                [sample(0), sample(99.8, False, 1), sample(100.01, False, 1)],
                None,
                1,
                100,
            )["state"],
            "scored_oom",
        )

    def test_post_cutoff_exit_plus_four_point_four(self):
        rows = [sample(0), sample(99.75), sample(100.001), sample(104.4, False, 1)]
        fail = {"elapsed_seconds": 104.4, "recorded_unix": 104.4}
        self.assertEqual(
            classify_lifetime(rows, fail, 1, 100)["state"], "post_cutoff_failure"
        )

    def test_positive_oom_first_observed_across_cutoff(self):
        self.assertEqual(
            classify_lifetime(
                [sample(0), sample(99.75), sample(100.001, False, 1)], None, 1, 100
            )["state"],
            "cutoff_unresolved",
        )

    def test_stale_cutoff_sample_cannot_prove_zero(self):
        self.assertEqual(
            classify_lifetime(
                [sample(0), sample(99.75), sample(100.001, True, 0, False)],
                None,
                1,
                100,
            )["state"],
            "cutoff_unresolved",
        )

    def test_qdisc_bounds(self):
        def q(n):
            return f"Sent 100 bytes 2 pkt (dropped {n}, overlimits 0 requeues 0)"

        original = {
            "private_router_qdiscs_after": {"rserver": q(1), "rclient": q(0)},
            "private_server_qdisc_after": q(0),
            "private_client_qdisc_after": q(0),
        }
        proof = {
            "qdisc_before": {
                n: q(0) for n in ["tserver", "tclient", "rserver", "rclient"]
            }
        }
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)

            def measure(phase, start, end, val):
                dump(
                    out / f"qdisc-{phase}-cutoff.json",
                    {
                        "cutoff_unix": 100,
                        "interfaces": {
                            "rserver": {
                                "started": start,
                                "finished": end,
                                "stats": q(val),
                            }
                        },
                    },
                )

            self.assertEqual(
                qdisc_window(out, original, proof, 100)["rserver"]["state"],
                "cutoff_unresolved",
            )
            measure("before", 99, 99.1, 1)
            self.assertEqual(
                qdisc_window(out, original, proof, 100)["rserver"]["state"],
                "scored_drop",
            )
            measure("before", 99, 99.1, 0)
            measure("after", 99.99, 100.01, 0)
            self.assertEqual(
                qdisc_window(out, original, proof, 100)["rserver"]["state"],
                "cutoff_unresolved",
            )
            measure("after", 100.01, 100.02, 0)
            self.assertEqual(
                qdisc_window(out, original, proof, 100)["rserver"]["state"],
                "post_cutoff_drop",
            )
            original["private_router_qdiscs_after"]["rserver"] = ""
            self.assertEqual(
                qdisc_window(out, original, proof, 100)["rserver"]["state"],
                "cutoff_unresolved",
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
