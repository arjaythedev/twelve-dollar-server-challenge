import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from summarize import check_relay_schedule, host_observations, request_histogram, stock_participation, summarize
from standings import row


class Standings(unittest.TestCase):
    def test_stock_pass_with_a_short_connection_window_counts_at_its_actual_load(self):
        trials = [
            {"users": 320000, "stock_pass": True, "qualified": True, "p99_ms": 494},
            {"users": 325000, "stock_pass": True, "qualified": False, "p99_ms": 757,
             "hold_full_connection_ticks": 58, "hold_expected_connection_ticks": 59},
            {"users": 330000, "stock_pass": False, "qualified": False, "p99_ms": 1836},
        ]
        result = row("Mojo", 304000, trials, 325000)
        self.assertIn("| 325,000 (1/1) | 320,000 (1/1) |", result)
        self.assertIn("757 ms (stock pass, 58/59 full checks)", result)


class ComparisonExport(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.run_dir = Path(self.temporary.name)
        self.write("shape.json", {})

    def write(self, name, data):
        (self.run_dir / name).write_text(json.dumps(data))

    def test_intermediate_qualification_is_not_a_final_score(self):
        self.write("summary.json", {"qualified": True})
        with self.assertRaisesRegex(ValueError, "assessment.*pending"):
            summarize(self.run_dir, {})

    def test_failed_preflight_is_recorded_without_a_load(self):
        self.write("summary.json", {"preflight": {"passed": False}, "qualified": False})
        row, _ = summarize(self.run_dir, {})
        self.assertEqual(row["assessment"], "preflight_failed")
        self.assertIsNone(row["stock_pass"])
        self.assertIsNone(request_histogram(self.run_dir))

    def test_old_network_assessment_is_checked_against_actual_scheduler_samples(self):
        relay = {
            "scheduler_observation": {"external_relay_workers": "proc"},
            "verified_workers": [{"tid": 3, "cpu": 2, "role": "reader"}],
        }
        self.write("timing.json", {"stock_start_unix": 0, "hold_starts_unix": 40, "cutoff_unix": 90})
        path = self.run_dir / "network-samples.jsonl"

        def samples(times, wait_fraction):
            points = []
            for t in times:
                state = {"run_ns": t * (1 - wait_fraction) * 1e9,
                         "runqueue_wait_ns": t * wait_fraction * 1e9, "timeslices": t}
                points.append({"unix": t, "polling_relay": {"loops": t},
                               "relay_worker_scheduling": {"3": state}})
            path.write_text("".join(json.dumps(p) + "\n" for p in points))

        samples(range(0, 90, 10), 0.3)
        old = {"stock_pass": True, "qualified": True, "scored_external_queue_evidence": True}
        evidence = check_relay_schedule(self.run_dir, relay, old)
        self.assertEqual(evidence, {"complete": True, "maximum_runnable_wait_pct": {"ramp": 30, "hold": 30}})
        self.assertTrue(old["stock_pass"])
        self.assertFalse(old["qualified"])
        self.assertEqual(len(old["infrastructure_flags"]), 2)

        samples(range(0, 90, 10), 0.01)
        already_excluded = {"qualified": False, "infrastructure_flags": ["service TAP loss"]}
        self.assertTrue(check_relay_schedule(self.run_dir, relay, already_excluded)["complete"])
        self.assertEqual(already_excluded, {"qualified": False, "infrastructure_flags": ["service TAP loss"]})

        samples(range(0, 45, 10), 0.01)
        incomplete = {"qualified": True, "scored_external_queue_evidence": True}
        self.assertFalse(check_relay_schedule(self.run_dir, relay, incomplete)["complete"])
        self.assertFalse(incomplete["qualified"])
        self.assertFalse(incomplete["scored_external_queue_evidence"])

    def test_assessed_run_requires_all_four_shards(self):
        self.write("summary.json", {"stock_pass": False, "scored_external_queue_evidence": False})
        self.write("merged.json", {})
        for i in range(3):
            self.write(f"shard-{i}.json", {})
        with self.assertRaisesRegex(ValueError, "four stock shard"):
            summarize(self.run_dir, {})
        self.write("shard-3.json", {})
        row, _ = summarize(self.run_dir, {})
        self.assertEqual(row["assessment"], "scored")
        self.assertFalse(row["stock_pass"])
        self.assertFalse(row["scored_external_queue_evidence"])

    def test_rps_result_requires_the_private_queue_assessment(self):
        self.write("shape.json", {"topology": "single-queue-split-polling-tap-external-txring4096-ordinary-rps"})
        result = {"stock_pass": True, "scored_external_queue_evidence": True, "qualified": False}
        self.write("summary.json", result)
        with self.assertRaisesRegex(ValueError, "steering assessment is pending"):
            summarize(self.run_dir, {})
        for index in range(4):
            self.write(f"shard-{index}.json", {})
        self.write("merged.json", {})
        result["target_network_flags"] = ["service CPU receive backlog lost packets"]
        result["softirq_rps_assessment"] = {"facts": {}, "infrastructure_flags": [],
                                           "target_network_flags": result["target_network_flags"]}
        self.write("summary.json", result)
        row, _ = summarize(self.run_dir, {})
        self.assertTrue(row["stock_pass"])
        self.assertFalse(row["qualified"])
        self.assertEqual(row["target_network_flags"], result["target_network_flags"])
        self.assertEqual(row["host_observations"]["service_receive_steering"], result["softirq_rps_assessment"])

    def test_merged_histogram_uses_request_counts(self):
        shards = [[19, 1, 0], [19, 1, 0], [19, 0, 1], [19, 0, 1]]
        for index, counts in enumerate(shards):
            self.write(f"shard-{index}.json", {"hist": counts})
        merged = {
            "hist": [76, 2, 2],
            "p95_ms": 1.01 ** 0.5 / 1000,
            "p99_ms": 1.01 ** 2.5 / 1000,
            "reqs": 81,
            "endpoints": [],
        }
        self.write("merged.json", merged)
        row = request_histogram(self.run_dir)
        self.assertEqual(row["counts"], [76, 2, 2])
        self.assertEqual(row["histogram_observations"], 80)
        self.assertEqual(row["endpoint_observations"], 81)
        self.write("shard-3.json", {"hist": [18, 1, 1]})
        with self.assertRaisesRegex(ValueError, "differs from the shard counts"):
            request_histogram(self.run_dir)

    def test_each_shard_needs_one_full_sample_at_the_same_tick(self):
        shape = {"users": 5, "ramp_s": 7, "hold_s": 20}
        shards = []
        for users in (2, 1, 1, 1):
            shards.append({
                "users": users, "dials": users,
                "timeline": [{"t": t, "users": users, "conns": users} for t in (14, 20, 26)],
            })
        initial = stock_participation(shards, shape)
        self.assertEqual(initial["expected_ticks"], 3)
        self.assertEqual(initial["incomplete_or_ambiguous_ticks"], [])
        self.assertEqual(initial["assigned"], [2, 1, 1, 1])
        # The total can still be five with uneven shard counts, or an ambiguous tick.
        shards[0]["timeline"][0]["conns"] -= 1
        shards[1]["timeline"][0]["conns"] += 1
        shards[2]["timeline"].append(dict(shards[2]["timeline"][1]))
        shards[3]["dials"] += 1
        changed = stock_participation(shards, shape)
        self.assertEqual(changed["incomplete_or_ambiguous_ticks"], [15, 20])
        self.assertEqual(changed["actual_dials"], [2, 1, 1, 2])
        self.assertEqual(changed["reported_users"], [2, 1, 1, 1])

    def test_memory_after_cutoff_keeps_its_own_sampling_window(self):
        self.write("timing.json", {"cutoff_unix": 120.5})
        first = {"after_cutoff_s": 0.1, "memory.peak": 40}
        last = {"after_cutoff_s": 16.3, "memory.peak": 90, "memory.events": {"oom": 0}}
        self.write("teardown-memory.json", {
            "cutoff_unix": 120.5,
            "first": first, "last": last, "observations": [first, last],
            "max_cgroup_peak_bytes": 90,
        })
        observation = host_observations(self.run_dir, {})["memory_after_stock_cutoff"]
        self.assertEqual(observation["maximum_cumulative_peak_bytes"], 90)
        self.assertEqual(observation["first_sample_s"], 0.1)
        self.write("timing.json", {"cutoff_unix": 140.0})
        with self.assertRaisesRegex(ValueError, "different cutoff"):
            host_observations(self.run_dir, {})

    def test_memory_profile_waits_for_its_additional_assessment(self):
        self.write("shape.json", {"topology": "local-private-passive-tcp-memory"})
        row = {"stock_pass": False, "scored_external_queue_evidence": True}
        self.write("summary.json", row)
        for index in range(4):
            self.write(f"shard-{index}.json", {})
        self.write("merged.json", {})
        self.write("service-controls.json", {"original_leaf_memory_max": 1750 * 1024 ** 2})
        with self.assertRaisesRegex(ValueError, "memory control assessment is pending"):
            summarize(self.run_dir, {})
        row["passive_tcp_memory_status"] = "service-oom"
        self.write("summary.json", row)
        self.write("passive-memory-result.json", {"oom_kill_at_cutoff": 1, "fixture_flags": []})
        result, _ = summarize(self.run_dir, {})
        self.assertEqual(result["passive_tcp_memory_status"], "service-oom")
        self.assertEqual(result["host_observations"]["passive_tcp_memory"]["oom_kill_at_cutoff"], 1)

    def test_quiet_control_uses_the_stock_clock_and_sampled_socket_counts(self):
        timing = {"stock_start_unix": 1, "hold_starts_unix": 5, "cutoff_unix": 21}
        self.write("timing.json", timing)
        self.write("component-test-lock.json", {"acquired_unix": 0.5, "finished_unix": 21.5})
        rows = [
            {"unix": t, "Tcp": {"CurrEstab": 100}, "host_sockstat_tcp": {"alloc": n}}
            for t, n in ((4, 900), (10, 202), (15, 204), (21, 800))
        ]
        (self.run_dir / "telemetry.jsonl").write_text("".join(json.dumps(row) + "\n" for row in rows))
        obs = host_observations(self.run_dir, {"host_checks": {"timing": timing}})
        self.assertTrue(obs["component_test_lock_covered_score"])
        self.assertEqual(obs["host_alloc_minus_twice_service_established"],
                         {"median": 3, "maximum": 4, "samples": 2})
        self.write("component-test-lock.json", {"acquired_unix": 2.0, "finished_unix": 21.5})
        self.assertFalse(host_observations(self.run_dir, {})["component_test_lock_covered_score"])

    def test_separate_disk_requires_the_database_on_the_measured_device(self):
        self.write("summary.json", {"stock_pass": True, "scored_external_queue_evidence": True})
        self.write("timing.json", {"stock_start_unix": 10.0, "cutoff_unix": 20.0})
        for index in range(4):
            self.write(f"shard-{index}.json", {})
        self.write("merged.json", {})
        volume = {
            "filesystem": {"maj:min": "1:2"}, "sample_cpu": 9,
            "samples": [
                {"unix": 14, "available_bytes": 150, "feed.db": {"device": "1:2"}},
                {"unix": 18, "available_bytes": 120,
                 "feed.db-wal": {"device": "1:2", "allocated": 30}},
                {"unix": 22, "available_bytes": 30},
            ],
        }
        self.write("database-volume.json", volume)
        row, fixture = summarize(self.run_dir, {})
        self.assertEqual(fixture["database_filesystem"]["maj:min"], "1:2")
        obs = row["host_observations"]["database_volume"]
        self.assertEqual((obs["minimum_available_bytes"], obs["peak_sampled_wal_allocated_bytes"]), (120, 30))
        self.assertEqual((obs["database_samples"], obs["wal_samples"]), (1, 1))
        self.assertEqual((obs["first_scored_sample_s"], obs["last_scored_sample_s"]), (4.0, 8.0))
        volume["samples"][1]["feed.db-wal"]["device"] = "3:4"
        self.write("database-volume.json", volume)
        with self.assertRaisesRegex(ValueError, "different block device"):
            summarize(self.run_dir, {})
        del volume["samples"][1]["feed.db-wal"]
        self.write("database-volume.json", volume)
        row, _ = summarize(self.run_dir, {})
        self.assertIsNone(row["host_observations"]["database_volume"]["peak_sampled_wal_allocated_bytes"])
        del volume["samples"][0]["feed.db"]
        self.write("database-volume.json", volume)
        with self.assertRaisesRegex(ValueError, "database file was never observed"):
            summarize(self.run_dir, {})


if __name__ == "__main__":
    unittest.main()
