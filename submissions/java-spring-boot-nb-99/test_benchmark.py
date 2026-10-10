"""Tests use fixed, reviewed k6 output values and explicit search traces."""

import io
import json
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest.mock import patch

import benchmark
from benchmark_report import report


def summary():
    return {
        "metrics": {
            "http_req_duration": {
                "p(95)": 1.65,
                "p(99)": 2.24,
                "avg": 1.04,
                "max": 33.68,
                "thresholds": {"p(95)<500": False, "p(99)<1000": False},
            },
            "http_req_failed": {"value": 0, "thresholds": {"rate<0.01": False}},
            "http_reqs": {"count": 93317, "rate": 233.3},
            "vus_max": {"value": 2500},
            "vus": {"max": 2500},
        },
        "root_group": {
            "checks": {
                "feed 200": {"passes": 43011, "fails": 0},
                "post 200": {"passes": 43011, "fails": 0},
                "like 200/201": {"passes": 6429, "fails": 0},
                "create 201": {"passes": 866, "fails": 0},
            }
        },
    }


class SummaryTest(unittest.TestCase):
    def test_requested_users_and_stage_time_must_have_run(self):
        benchmark.parse_summary(summary(), 0, expected_vus=2500, minimum_seconds=390)
        for users, seconds in ((5000, 390), (2500, 500)):
            with (
                self.subTest(users=users, seconds=seconds),
                self.assertRaises(benchmark.ExecutionError),
            ):
                benchmark.parse_summary(
                    summary(), 0, expected_vus=users, minimum_seconds=seconds
                )

    def test_inconsistent_threshold_flags_are_rejected(self):
        for flag in (True, "false", {"ok": True}):
            data = summary()
            data["metrics"]["http_req_duration"]["thresholds"]["p(95)<500"] = flag
            with self.subTest(flag=flag), self.assertRaises(benchmark.ExecutionError):
                benchmark.parse_summary(data, 0)

    def test_runtime_error_is_not_ignored_alongside_threshold_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "events.jsonl"
            path.write_text(
                json.dumps(
                    {
                        "level": "error",
                        "msg": "thresholds on metrics 'http_req_duration' have been crossed",
                    }
                )
                + "\n"
                + json.dumps({"level": "error", "msg": "TypeError: bad response"})
                + "\n"
            )
            with self.assertRaises(benchmark.ExecutionError):
                benchmark.validate_events(path, 99)

    def test_runtime_exception_is_not_ignored_even_with_exit_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "events.jsonl"
            path.write_text('{"level":"error","msg":"TypeError: bad response"}\n')
            with self.assertRaises(benchmark.ExecutionError):
                benchmark.validate_events(path, 0)

    def test_threshold_event_is_only_allowed_for_threshold_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "events.jsonl"
            path.write_bytes(
                (
                    Path(__file__).parent / "benchmark-fixtures/threshold.events.jsonl"
                ).read_bytes()
            )
            benchmark.validate_events(path, 99)
            with self.assertRaises(benchmark.ExecutionError):
                benchmark.validate_events(path, 0)

    def test_successful_export_uses_failed_booleans(self):
        result = benchmark.parse_summary(summary(), 0)
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["metrics"]["requests"], 93317)
        self.assertEqual(
            result["thresholds"],
            {"p(95)<500": True, "p(99)<1000": True, "rate<0.01": True},
        )
        self.assertEqual(result["checks"]["create 201"], {"passed": 866, "failed": 0})

    def test_exact_threshold_boundary_fails(self):
        data = summary()
        data["metrics"]["http_req_duration"]["p(95)"] = 500
        data["metrics"]["http_req_duration"]["thresholds"]["p(95)<500"] = True
        result = benchmark.parse_summary(data, 99)
        self.assertEqual(result["status"], "threshold failed")
        self.assertFalse(result["thresholds"]["p(95)<500"])

    def test_execution_failures_do_not_become_capacity_failures(self):
        for code in (1, 2, 98, 137, 143, -9):
            with self.subTest(code=code), self.assertRaises(benchmark.ExecutionError):
                benchmark.parse_summary(summary(), code)

    def test_inconsistent_exit_and_missing_summary_rejected(self):
        for data, code in ((summary(), 99), ({}, 0)):
            with self.assertRaises(benchmark.ExecutionError):
                benchmark.parse_summary(data, code)

    def test_zero_work_and_invalid_numbers_rejected(self):
        for value in (0, -1, float("nan"), float("inf"), True, "93317"):
            data = summary()
            data["metrics"]["http_reqs"]["count"] = value
            with self.subTest(value=value), self.assertRaises(benchmark.ExecutionError):
                benchmark.parse_summary(data, 0)

    def test_check_failure_is_separate_from_threshold_result(self):
        data = summary()
        data["root_group"]["checks"]["like 200/201"] = {"passes": 6428, "fails": 1}
        result = benchmark.parse_summary(data, 0)
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["checks"]["like 200/201"]["failed"], 1)

    def test_missing_write_check_is_unexercised(self):
        data = summary()
        del data["root_group"]["checks"]["create 201"]
        self.assertEqual(
            benchmark.parse_summary(data, 0)["checks"]["create 201"],
            {"passed": 0, "failed": 0},
        )

    def test_missing_feed_work_is_execution_error(self):
        data = summary()
        del data["root_group"]["checks"]["feed 200"]
        with self.assertRaises(benchmark.ExecutionError):
            benchmark.parse_summary(data, 0)


class SearchTest(unittest.TestCase):
    def test_doubles_refines_and_confirms(self):
        calls = []

        def run(phase, users):
            calls.append((phase, users))
            return users <= 3250

        self.assertEqual(benchmark.search(run, 2500, 20000, 250, 1), (3250, False))
        self.assertEqual(
            calls,
            [
                ("search", 2500),
                ("search", 5000),
                ("refine", 3750),
                ("refine", 3000),
                ("refine", 3250),
                ("refine", 3500),
                ("confirm", 3250),
            ],
        )

    def test_failed_first_probe_searches_lower(self):
        self.assertEqual(
            benchmark.search(lambda _phase, users: users <= 1000, 2500, 20000, 250, 1),
            (1000, False),
        )

    def test_cap_is_not_reported_as_maximum(self):
        calls = []
        self.assertEqual(
            benchmark.search(
                lambda phase, users: calls.append((phase, users)) or True,
                2500,
                5000,
                250,
                2,
            ),
            (5000, True),
        )
        self.assertEqual(
            calls,
            [("search", 2500), ("search", 5000), ("confirm", 5000), ("confirm", 5000)],
        )

    def test_confirmation_failure_steps_down_and_restarts_repetitions(self):
        calls = []

        def run(phase, users):
            calls.append((phase, users))
            return phase != "confirm" or users < 500

        self.assertEqual(benchmark.search(run, 500, 500, 250, 2), (250, False))
        self.assertEqual(
            calls,
            [("search", 500), ("confirm", 500), ("confirm", 250), ("confirm", 250)],
        )

    def test_no_pass_is_not_zero_user_success(self):
        self.assertEqual(
            benchmark.search(lambda _phase, _users: False, 2500, 20000, 250, 1),
            (None, False),
        )

    def test_execution_error_and_budget_stop_search(self):
        for exception in (benchmark.ExecutionError, benchmark.BudgetExpired):

            def run(_phase, _users, exception=exception):
                raise exception("stop")

            with self.assertRaises(exception):
                benchmark.search(run, 2500, 20000, 250, 1)


class ReportTest(unittest.TestCase):
    def test_short_confirmation_and_unexercised_check_are_not_passes(self):
        state = self.state()
        state["environment"]["settings"] = {"confirm_seconds": 10}
        state["runs"][0]["checks"]["create 201"] = {"passed": 0, "failed": 0}
        with tempfile.TemporaryDirectory() as directory:
            report(directory, state)
            md = (Path(directory) / "report.md").read_text()
            self.assertIn("Smoke-test baseline", md)
            self.assertIn("not fully exercised", md)

    def state(self):
        run = {
            "id": 1,
            "phase": "confirm",
            "vus": 2500,
            "hold_seconds": 300,
            "started_at": "2026-10-07T15:00:00Z",
            "wall_seconds": 400,
            "exit_code": 0,
            "log": "run-001.log",
            "summary": "run-001.json",
            **benchmark.parse_summary(summary(), 0),
        }
        return {
            "runs": [run],
            "confirmed_vus": 2500,
            "status": "completed",
            "environment": {"host": "test"},
            "message": "Search finished.",
        }

    def test_reports_link_evidence_and_embed_charts_without_cdn(self):
        with tempfile.TemporaryDirectory() as directory:
            report(directory, self.state())
            md = (Path(directory) / "report.md").read_text()
            page = (Path(directory) / "report.html").read_text()
            self.assertIn("Confirmed local baseline: 2,500 virtual users.", md)
            self.assertIn("create 201 | passed: 866 passed, 0 failed", md)
            self.assertIn("[Raw k6 summary](run-001.json)", md)
            self.assertEqual(page.count("<svg "), 4)
            self.assertNotIn("<script", page)
            self.assertIn("Limit 500", page)
            self.assertTrue((Path(directory) / "errors.svg").is_file())
            for filename in ("p95.svg", "p99.svg", "throughput.svg", "errors.svg"):
                svg = ET.fromstring((Path(directory) / filename).read_bytes())
                self.assertEqual(svg.tag, "{http://www.w3.org/2000/svg}svg")

    def test_html_escapes_untrusted_log_error_and_environment(self):
        state = self.state()
        state["runs"][0]["error"] = '<script>alert("x")</script>'
        state["environment"]["host"] = "<img onerror=x>"
        with tempfile.TemporaryDirectory() as directory:
            report(directory, state)
            page = (Path(directory) / "report.html").read_text()
            self.assertNotIn("<script", page)
            self.assertNotIn("<img onerror", page)
            self.assertIn("&lt;script&gt;", page)

    def test_failed_unexercised_and_execution_error_are_visible(self):
        state = self.state()
        state["runs"][0]["checks"]["create 201"] = {"passed": 0, "failed": 0}
        state["runs"][0]["checks"]["like 200/201"] = {"passed": 4, "failed": 1}
        state["runs"].append(
            {
                "id": 2,
                "phase": "search",
                "vus": 5000,
                "hold_seconds": 60,
                "started_at": "now",
                "status": "execution error",
                "log": "run-002.log",
                "error": "No summary",
            }
        )
        with tempfile.TemporaryDirectory() as directory:
            report(directory, state)
            md = (Path(directory) / "report.md").read_text()
            self.assertIn("create 201 | not exercised: 0 passed, 0 failed", md)
            self.assertIn("like 200/201 | failed: 4 passed, 1 failed", md)
            self.assertIn("Outcome: execution error.", md)

    def test_empty_interrupted_report_is_valid(self):
        with tempfile.TemporaryDirectory() as directory:
            report(directory, {"runs": [], "status": "interrupted", "environment": {}})
            data = json.loads((Path(directory) / "results.json").read_text())
            self.assertEqual(data["runs"], [])


class ConfigTest(unittest.TestCase):
    def test_argument_bounds(self):
        for args in (
            ["--port", "65536"],
            ["--step", "0"],
            ["--start-vus", "2510"],
            ["--start-vus", "25000"],
            ["--budget-seconds", "-1"],
        ):
            with (
                self.subTest(args=args),
                patch("sys.stderr"),
                self.assertRaises(SystemExit),
            ):
                benchmark.arguments(args)

    def test_config_owns_its_volume_and_preserves_original_port(self):
        original = {
            "name": "isolated",
            "services": {
                "app": {"mem_limit": "1073741824"},
                "seed": {},
                "nginx": {"ports": [{"published": "3000"}]},
                "test": {},
            },
            "volumes": {"database": {"name": "isolated_database"}},
            "networks": {"default": {"name": "isolated_default"}},
        }
        with (
            tempfile.TemporaryDirectory() as directory,
            patch.object(benchmark, "command", return_value=json.dumps(original)),
        ):
            compose = benchmark.compose_config("isolated", Path(directory), 3001)
            config = json.loads((Path(directory) / "compose.json").read_text())
            self.assertEqual(compose[3], "isolated")
            self.assertEqual(config["volumes"]["database"]["name"], "isolated_database")
            self.assertEqual(
                config["services"]["nginx"]["ports"][0]["published"], "3001"
            )
            self.assertEqual(config["services"]["app"]["memswap_limit"], "1073741824")
            self.assertNotIn("test", config["services"])
            self.assertEqual(config["services"]["app"]["image"], "isolated-app:latest")
            self.assertEqual(
                config["services"]["k6"]["volumes"][-1]["source"],
                str(Path(directory) / "k6"),
            )
            self.assertEqual(
                original["services"]["nginx"]["ports"][0]["published"], "3000"
            )


class LifecycleTest(unittest.TestCase):
    def execute(self, mode):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        output = Path(temporary.name) / "new-results"
        commands = []
        compose = [
            "docker",
            "compose",
            "-p",
            "isolated-owned-project",
            "-f",
            "fixture.json",
        ]

        def command(args, **_kwargs):
            commands.append(args)
            if "k6" in args:
                return load(args)
            if mode == "cleanup error" and "down" in args:
                raise benchmark.ExecutionError("Docker cleanup unavailable")
            if mode == "load cleanup error" and args[:2] == ["docker", "ps"]:
                raise benchmark.ExecutionError("Container listing unavailable")
            if mode == "startup error" and "up" in args:
                raise benchmark.ExecutionError("Port unavailable")
            if args[:2] == ["docker", "inspect"]:
                return json.dumps(
                    [
                        {
                            "Image": "sha256:fixture",
                            "HostConfig": {
                                "NanoCpus": 1000000000,
                                "Memory": 1073741824,
                                "MemorySwap": 1073741824,
                            },
                        }
                    ]
                )
            if "ps" in args:
                return "container-id"
            return ""

        def load(args, **_kwargs):
            if mode == "interrupted":
                raise KeyboardInterrupt
            if mode == "timeout":
                raise subprocess.TimeoutExpired(args, 1)
            filename = next(arg for arg in args if arg.startswith("--summary-export="))
            data = summary()
            users = int(
                next(arg.split("=")[1] for arg in args if arg.startswith("VUS="))
            )
            data["metrics"]["vus_max"]["value"] = users
            data["metrics"]["vus"]["max"] = users
            (output / "k6" / filename.split("/")[-1]).write_text(json.dumps(data))
            eventfile = next(arg for arg in args if arg.startswith("--log-output="))
            (output / "k6" / eventfile.split("/")[-1]).write_text("")
            return 0

        resources = {
            "cpu_usec": 1000,
            "peak_memory_mib": 188,
            "oom_kills": 0,
            "restart_count": 0,
            "oom_killed": False,
            "running": True,
        }
        calls = 0

        def resource(*_args):
            nonlocal calls
            calls += 1
            data = dict(resources)
            if mode == "oom" and calls == 2:
                data["oom_kills"] = 1
            if mode == "unexpected error":
                raise RuntimeError("Unexpected diagnostic failure")
            return data

        args = [
            "--output",
            str(output),
            "--reuse-images",
            "--start-vus",
            "250",
            "--max-vus",
            "250",
            "--budget-seconds",
            "1" if mode == "budget" else "2000",
        ]

        def config(*_args):
            (output / "k6").mkdir()
            return compose

        with (
            patch.object(benchmark, "compose_config", side_effect=config),
            patch.object(benchmark, "command", side_effect=command),
            patch.object(benchmark, "health"),
            patch.object(benchmark, "app_resources", side_effect=resource),
            patch.object(benchmark.signal, "signal"),
            patch.object(benchmark.platform, "platform", return_value="fixture-host"),
            patch("sys.stdout", new_callable=io.StringIO),
        ):
            code = benchmark.main(args)
        return code, json.loads((output / "results.json").read_text()), commands, output

    def test_complete_search_keeps_evidence_and_cleans_only_owned_project(self):
        code, state, commands, output = self.execute("success")
        self.assertEqual(code, 0)
        self.assertEqual(state["status"], "capped")
        self.assertEqual(state["confirmed_vus"], 250)
        self.assertEqual(
            [run["phase"] for run in state["runs"]], ["warmup", "search", "confirm"]
        )
        self.assertTrue((output / "report.html").exists())
        self.assertTrue((output / "k6/run-003.json").exists())
        self.assertIn(
            [
                "docker",
                "compose",
                "-p",
                "isolated-owned-project",
                "-f",
                "fixture.json",
                "--profile",
                "bench",
                "down",
                "--volumes",
                "--remove-orphans",
            ],
            commands,
        )

    def test_budget_does_not_start_a_partial_run(self):
        code, state, commands, _output = self.execute("budget")
        self.assertEqual(code, 2)
        self.assertEqual(state["status"], "budget exhausted")
        self.assertEqual(state["runs"], [])
        self.assertTrue(any("down" in args for args in commands))

    def test_startup_error_is_reported_and_cleaned(self):
        code, state, commands, _output = self.execute("startup error")
        self.assertEqual(code, 2)
        self.assertEqual(state["status"], "execution error")
        self.assertIn("Port unavailable", state["message"])
        self.assertTrue(any("down" in args for args in commands))

    def test_timeout_records_budget_stop_instead_of_capacity_failure(self):
        code, state, commands, _output = self.execute("timeout")
        self.assertEqual(code, 2)
        self.assertEqual(state["status"], "budget exhausted")
        self.assertEqual(state["runs"][0]["status"], "budget exhausted")
        self.assertTrue(any("down" in args for args in commands))

    def test_interruption_retains_partial_report_and_cleans(self):
        code, state, commands, output = self.execute("interrupted")
        self.assertEqual(code, 130)
        self.assertEqual(state["status"], "interrupted")
        self.assertEqual(state["runs"][0]["status"], "interrupted")
        self.assertTrue((output / "report.md").exists())
        self.assertTrue(any("down" in args for args in commands))

    def test_cleanup_failure_is_retained_and_returns_execution_error(self):
        code, state, _commands, _output = self.execute("cleanup error")
        self.assertEqual(code, 2)
        self.assertIn("cleanup_error", state)
        self.assertIn("Cleanup failed", state["message"])

    def test_compose_teardown_is_attempted_when_load_cleanup_fails(self):
        code, state, commands, _output = self.execute("load cleanup error")
        self.assertEqual(code, 0)
        self.assertIn("load_cleanup_error", state)
        self.assertTrue(any("down" in args for args in commands))

    def test_oom_is_not_recorded_as_a_threshold_failure(self):
        code, state, _commands, _output = self.execute("oom")
        self.assertEqual(code, 2)
        self.assertEqual(state["runs"][0]["status"], "execution error")
        self.assertIn("OOM", state["message"])

    def test_unexpected_error_does_not_leave_running_status(self):
        code, state, _commands, output = self.execute("unexpected error")
        self.assertEqual(code, 2)
        self.assertEqual(state["status"], "execution error")
        self.assertEqual(state["runs"][0]["status"], "execution error")
        self.assertTrue((output / "run-001.traceback.txt").exists())

    def test_existing_output_directory_is_rejected_without_overwriting(self):
        with (
            tempfile.TemporaryDirectory() as directory,
            patch("sys.stderr", new_callable=io.StringIO),
        ):
            marker = Path(directory) / "existing.txt"
            marker.write_text("preserved")
            self.assertEqual(benchmark.main(["--output", directory]), 2)
            self.assertEqual(marker.read_text(), "preserved")


class ResourceTest(unittest.TestCase):
    def test_cgroup_text_and_inspect_are_normalized(self):
        inspected = json.dumps(
            [{"RestartCount": 0, "State": {"OOMKilled": False, "Running": True}}]
        )
        counters = "usage_usec 1000000\noom_kill 0\nmemory.peak 2097152\nmemory.swap.peak n/a\n"
        with patch.object(benchmark, "command", side_effect=[counters, inspected]):
            result = benchmark.app_resources([], "app")
        self.assertEqual(result["cpu_usec"], 1000000)
        self.assertEqual(result["peak_memory_mib"], 2)
        self.assertIsNone(result["swap_peak_mib"])
        self.assertTrue(result["running"])

    def test_truncated_or_malformed_resource_data_is_an_execution_error(self):
        for counters in ("", "usage_usec\n", "usage_usec invalid\n"):
            with (
                patch.object(benchmark, "command", side_effect=[counters, "[]"]),
                self.assertRaises(benchmark.ExecutionError),
            ):
                benchmark.app_resources([], "app")


if __name__ == "__main__":
    unittest.main()
