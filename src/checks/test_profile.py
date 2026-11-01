"""Check weighted native profiles, stack attribution and incomplete captures."""

import unittest

from dev import (
    JSONValue,
    analyze_profile,
    cpu_delta,
    duration_seconds,
    parse_counters,
    profile_stage,
)


def stack(period: int, *frames: tuple[str, str]) -> str:
    return (
        str(period) + "\n" + "\n".join(f"\t1234 {symbol} ({module})" for symbol, module in frames)
    )


def statistics(samples: int, lost: int = 0) -> str:
    return (
        f"Aggregated stats:\n SAMPLE events: {samples}\n LOST events: {lost}\n"
        f"cpu-clock:u stats:\n SAMPLE events: {samples}\n"
    )


class NativeProfileTests(unittest.TestCase):
    def test_process_name_does_not_shift_cpu_fields(self) -> None:
        fields = ["S", *(["0"] * 10), "123", "45", *(["0"] * 30)]
        values = "usage_usec 1700000\nuser_usec 1300000\nprocess_stat 1 (a name (x)) "
        counters = parse_counters(values + " ".join(fields))
        self.assertEqual(counters["usage_usec"], 1700000)
        self.assertEqual(counters["main_user_ticks"], 123)
        self.assertEqual(counters["main_system_ticks"], 45)

    def test_main_process_and_cgroup_cpu_remain_separate(self) -> None:
        before: dict[str, JSONValue] = {
            "user_usec": 1000000,
            "system_usec": 2000000,
            "main_user_ticks": 90,
            "main_system_ticks": 140,
        }
        after: dict[str, JSONValue] = {
            "user_usec": 3000000,
            "system_usec": 5000000,
            "main_user_ticks": 290,
            "main_system_ticks": 340,
        }
        result = cpu_delta(before, after, 100)
        self.assertEqual(result["cgroup_user_s"], 2)
        self.assertEqual(result["cgroup_system_s"], 3)
        self.assertEqual(result["main_system_s"], 2)
        self.assertEqual(result["cgroup_minus_main_s"], 1)

    def test_profile_durations(self) -> None:
        self.assertEqual(duration_seconds("30s"), 30)
        self.assertEqual(duration_seconds("1m30s"), 90)
        self.assertEqual(duration_seconds("500ms"), 0.5)
        for value in ("", "0s", "-1s", "60", "30sjunk", "infh"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                duration_seconds(value)

    def test_period_weights_and_unresolved_leaves(self) -> None:
        auth = stack(
            100,
            ("yyjson_read", "/app/server"),
            ("tds::authenticate(std::string_view, std::string_view)", "/app/server"),
            ("main", "/app/server"),
        )
        database = stack(
            300,
            ("[unknown]", "/lib/libc.so.6"),
            ("sqlite3_step", "/app/server"),
            ("main", "/app/server"),
        )
        report = analyze_profile(auth + "\n\n\n" + database, statistics(2))
        self.assertEqual(report["samples"], 2)
        self.assertEqual(report["sampled_cpu_ns"], 400)
        self.assertEqual(report["stages_cpu_ns"], {"auth": 100, "database_and_rows": 300})
        self.assertEqual(report["unresolved_leaf_samples"], 1)
        self.assertEqual(report["unrooted_samples"], 0)
        self.assertEqual(report["hotspots"][0]["cpu_ns"], 300)

    def test_json_writer_is_separate_from_database_parent(self) -> None:
        frames = [
            ("tds::quote(std::string&, std::string_view)", "/app/server"),
            ("tds::Database::feed[abi:cxx11]()", "/app/server"),
        ]
        self.assertEqual(profile_stage(frames), "response_json")
        self.assertEqual(profile_stage([("yyjson_read", "/app/server")]), "request_json")
        self.assertEqual(profile_stage([("[unknown]", "/lib/libcrypto.so.3")]), "auth")
        self.assertEqual(
            profile_stage([("llhttp__internal__run", "/app/server")]), "transport_and_other"
        )

    def test_incomplete_unresolved_stacks_stay_visible(self) -> None:
        report = analyze_profile(stack(10, ("[unknown]", "[unknown]")), statistics(1))
        self.assertEqual(report["stages_cpu_ns"], {"unknown": 10})
        self.assertEqual(report["unrooted_samples"], 1)
        self.assertEqual(report["unresolved_leaf_samples"], 1)

    def test_record_loss_and_sample_mismatches_fail(self) -> None:
        sample = stack(10, ("main", "/app/server"))
        with self.assertRaisesRegex(ValueError, "loss records"):
            analyze_profile(sample, statistics(1, lost=2))
        with self.assertRaisesRegex(ValueError, "sample count"):
            analyze_profile(sample, statistics(2))

    def test_empty_malformed_and_zero_period_samples_fail(self) -> None:
        for sample in ("", "10\ninvalid frame", stack(0, ("main", "/app/server")), "10"):
            with self.subTest(sample=sample), self.assertRaises(ValueError):
                analyze_profile(sample, statistics(1))
