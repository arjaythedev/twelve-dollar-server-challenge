"""Check weighted native profiles, stack attribution and incomplete captures."""

import unittest

from dev import analyze_profile, profile_stage


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
