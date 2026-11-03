"""Tests for dev tooling, workload orchestration and profile analysis"""

import json
import subprocess
import tempfile
import unittest
from dataclasses import FrozenInstanceError
from io import StringIO
from pathlib import Path
from types import TracebackType
from unittest.mock import patch

import dev
from devtools import reports, runtime
from devtools.checks import native_check
from devtools.reports import (
    JSONValue,
    Result,
    analyze_profile,
    append_result,
    cpu_delta,
    decode_json,
    duration_seconds,
    kernel_stage,
    mapping,
    parse_counters,
    parse_results,
    parse_telemetry,
    profile_stage,
    read_json,
    write_json,
)
from devtools.runtime import (
    GENERATOR_IMAGE,
    LoadGenerator,
    RunConfig,
    ensure_generator,
    generator_fingerprint,
    load,
    load_generator,
    run_workload,
    server,
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
    def test_cpp_workload_seed_reaches_the_generator(self) -> None:
        for seed in (None, 3132383413):
            with (
                self.subTest(seed=seed),
                tempfile.TemporaryDirectory() as directory,
                patch(
                    "devtools.runtime.command",
                    side_effect=["127.0.0.1:6565", "cpp-version\nsha /app/load"],
                ),
                patch(
                    "devtools.runtime.subprocess.run",
                    return_value=subprocess.CompletedProcess([], 0, "[]", ""),
                ),
            ):
                output = Path(directory)
                write_json(output / "environment.json", {"docker_info": {"MemTotal": 24 * 2**30}})
                args = RunConfig(
                    label="unit-test",
                    generator="cpp",
                    k6=None,
                    vus=1,
                    hold="60s",
                    ramp="30s",
                    seed=seed,
                )
                with load_generator(args, "test-container", 3000, output) as generator:
                    self.assertIn(GENERATOR_IMAGE, generator.invocation)
                    if seed is None:
                        self.assertNotIn("--seed", generator.invocation)
                    else:
                        index = generator.invocation.index("--seed")
                        self.assertEqual(generator.invocation[index + 1], str(seed))

    def test_benchmark_append_preserves_experiment_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.json"
            previous: JSONValue = {
                "name": "earlier",
                "server_sha256": "original",
                "experiment": {"hold": [1, 2], "wal_cache_eviction": False},
            }
            result = Result(
                date="2026-11-03",
                name="next",
                runtime="cpp",
                load_generator="docker_loopback",
                sqlite="3.53.1",
                users=1,
                hold="60s",
                ramp="30s",
                requests=1,
                rps=1.0,
                p95_ms=1.0,
                p99_ms=1.0,
                max_ms=1.0,
                error_rate=0.0,
                cpu_us_per_request=1.0,
                cpu_scope="server_cgroup",
                server_peak_bytes=1,
                passed=True,
            )
            append_result(path, result)
            self.assertEqual(read_json(path), [result])
            write_json(path, [previous])
            append_result(path, result)
            self.assertEqual(read_json(path), [previous, result])
            write_json(path, {"runs": [result], "experiment": {"hold": [1, 2]}})
            self.assertEqual(parse_results(read_json(path)), [result])
            write_json(path, {"invalid": "root"})
            with self.assertRaisesRegex(ValueError, "JSON array"):
                append_result(path, result)
            self.assertEqual(read_json(path), {"invalid": "root"})

    def test_combined_profile_separates_modes_and_retains_user_caller_frames(self) -> None:
        user = stack(100, ("sqlite3_step", "/app/server"), ("main", "/app/server"))
        kernel = stack(
            300,
            ("posix_lock_inode", "[kernel.kallsyms]"),
            ("__x64_sys_fcntl", "[kernel.kallsyms]"),
            ("sqlite3_step", "/app/server"),
            ("main", "/app/server"),
        )
        report = analyze_profile(
            user + "\n\n" + kernel,
            statistics(2).replace("cpu-clock:u", "cpu-clock"),
            scope="all",
        )
        self.assertEqual(report["event"], "cpu-clock")
        self.assertEqual(report["sampled_cpu_ns"], 400)
        self.assertEqual(
            report["stages_cpu_ns"], {"database_and_rows": 100, "kernel_filesystem": 300}
        )
        self.assertEqual(report.get("syscalls_cpu_ns"), {"fcntl": 300})
        self.assertEqual(report["unrooted_samples"], 0)

    def test_kernel_attribution_preserves_nested_network_and_file_work(self) -> None:
        outer = ("__x64_sys_io_uring_enter", "[kernel.kallsyms]")
        self.assertEqual(
            kernel_stage([("tcp_sendmsg_locked", "[kernel.kallsyms]"), outer]),
            "kernel_network",
        )
        self.assertEqual(
            kernel_stage([("do_fcntl", "[kernel.kallsyms]"), outer]), "kernel_filesystem"
        )
        report = analyze_profile(
            stack(300, outer) + "\n\n" + stack(100, ("[unknown]", "[unknown]")),
            statistics(2).replace("cpu-clock:u", "cpu-clock:k"),
            scope="kernel",
        )
        self.assertEqual(report["event"], "cpu-clock:k")
        self.assertEqual(report["stages_cpu_ns"], {"kernel_io_uring": 300, "unknown": 100})
        self.assertEqual(report["unrooted_samples"], 1)
        self.assertEqual(report.get("syscalls_cpu_ns"), {"io_uring_enter": 300, "other": 100})

    def test_finished_generator_can_be_reaped_after_sampler_race(self) -> None:
        args = RunConfig(vus=1, mode="bench", label="exit-race", ramp="30s", hold="60s")
        generator = LoadGenerator(["k6"], 6565, "test", "test", "load-container")
        snapshot: dict[str, JSONValue] = {
            "user_usec": 0,
            "system_usec": 0,
            "main_user_ticks": 0,
            "main_system_ticks": 0,
        }
        with (
            tempfile.TemporaryDirectory() as directory,
            patch("devtools.runtime.subprocess.Popen") as launch,
            patch("devtools.runtime.command", return_value="100"),
            patch("devtools.runtime.k6_status", return_value={"status": 4}),
            patch("devtools.runtime.counters", return_value=snapshot),
            patch.object(
                LoadGenerator, "sample", side_effect=subprocess.CalledProcessError(1, "docker exec")
            ),
        ):
            child = launch.return_value.__enter__.return_value
            child.pid = 1
            child.wait.return_value = 0

            def poll() -> int | None:
                return 0 if child.wait.called else None

            child.poll.side_effect = poll
            self.assertEqual(load(args, generator, "observer", Path(directory)), 0)
            child.wait.assert_any_call(timeout=15)
            child.kill.assert_not_called()
            self.assertTrue((Path(directory) / "run.json").is_file())

    def test_cpp_generator_starts_through_gate_without_k6_api(self) -> None:
        args = RunConfig(vus=1, mode="bench", label="native-gate", ramp="30s", hold="60s")
        generator = LoadGenerator(["load_generator"], 0, "test", "test", kind="cpp")
        snapshot: dict[str, JSONValue] = {
            "user_usec": 0,
            "system_usec": 0,
            "main_user_ticks": 0,
            "main_system_ticks": 0,
        }
        with (
            tempfile.TemporaryDirectory() as directory,
            patch("devtools.runtime.subprocess.Popen") as launch,
            patch("devtools.runtime.command", return_value="100"),
            patch("devtools.runtime.k6_status") as k6_api,
            patch("devtools.runtime.counters", return_value=snapshot),
            patch.object(LoadGenerator, "sample", return_value={}),
        ):
            output = Path(directory)
            (output / "generator.ready").touch()
            child = launch.return_value.__enter__.return_value
            child.pid = 1
            child.wait.return_value = 0
            child.poll.side_effect = lambda: 0 if (output / "generator.start").exists() else None
            self.assertEqual(load(args, generator, "observer", output), 0)
            k6_api.assert_not_called()
            self.assertEqual(
                mapping(decode_json((output / "run.json").read_bytes()))["generator"], "cpp"
            )

    def test_process_name_does_not_shift_cpu_fields(self) -> None:
        fields = ["S", *(["0"] * 10), "123", "45", *(["0"] * 30)]
        values = (
            "usage_usec 1700000\nuser_usec 1300000\n"
            "memory_current 65536\nmemory_stat_anon 8192\n"
            "memory_stat_file 32768\nmemory_stat_sock 4096\n"
            "Tcp: InSegs OutSegs RetransSegs\nTcp: 100 200 3\n"
            "TcpExt: ListenDrops TCPTimeouts\nTcpExt: 5 7\n"
            "io_pressure some avg10=2.40 avg60=1.00 avg300=0.20 total=321000\n"
            "io_pressure full avg10=1.20 avg60=0.50 avg300=0.10 total=123000\n"
            "process_stat 1 (a name (x)) "
        )
        counters = parse_counters(values + " ".join(fields))
        self.assertEqual(counters["usage_usec"], 1700000)
        self.assertEqual(counters["memory_current"], 65536)
        self.assertEqual(counters["memory_stat_anon"], 8192)
        self.assertEqual(counters["memory_stat_file"], 32768)
        self.assertEqual(counters["memory_stat_sock"], 4096)
        self.assertEqual(counters["net_tcp_RetransSegs"], 3)
        self.assertEqual(counters["net_tcpext_ListenDrops"], 5)
        self.assertEqual(counters["net_tcpext_TCPTimeouts"], 7)
        self.assertEqual(counters["main_user_ticks"], 123)
        self.assertEqual(counters["main_system_ticks"], 45)
        self.assertEqual(counters["io_pressure_some_usec"], 321000)
        self.assertEqual(counters["io_pressure_full_usec"], 123000)

    def test_stop_timeout_preserves_failure_and_removes_owned_resources(self) -> None:
        def response(*args: str) -> str:
            return "127.0.0.1:12345" if args[:2] == ("docker", "port") else "{}"

        with (
            tempfile.TemporaryDirectory() as directory,
            patch("devtools.runtime.command", side_effect=response) as command,
            patch("devtools.runtime.healthy", return_value=True),
            patch("devtools.runtime.subprocess.run") as run,
            patch("builtins.print"),
        ):
            output = Path(directory)
            seed = output / "fixture.db"
            seed.touch()
            run.side_effect = [
                subprocess.TimeoutExpired("docker stop", 20),
                subprocess.CompletedProcess(["docker", "inspect"], 0, "[]", ""),
                subprocess.CompletedProcess(["docker", "logs"], 0, "Listening\n", ""),
                subprocess.CompletedProcess(["docker", "rm"], 0, "", ""),
            ]
            container_name = ""
            with self.assertRaisesRegex(RuntimeError, "request failed"):
                with server(output, database=seed) as (name, port):
                    container_name = name
                    self.assertEqual(port, 12345)
                    raise RuntimeError("request failed")
            run.assert_any_call(
                ["docker", "rm", "--force", container_name], capture_output=True, check=False
            )
            command.assert_any_call("docker", "volume", "rm", container_name + "-data")
            self.assertEqual((output / "server.log").read_text(), "Listening\n")

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
        self.assertEqual(
            profile_stage([("tds::Store::feed(tds::PostWriter&)", "/app/server")]),
            "database_and_rows",
        )
        self.assertEqual(
            profile_stage(
                [
                    ("tds::PostWriter::append(...) ", "/app/server"),
                    ("tds::Store::feed(tds::PostWriter&)", "/app/server"),
                ]
            ),
            "response_json",
        )
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


class NativeRunnerTests(unittest.TestCase):
    def test_ctest_owns_the_native_test_list_and_has_writable_logs(self) -> None:
        with (
            patch("devtools.runtime.build", return_value="native-tests") as build,
            patch("devtools.checks.cpp_editor"),
            patch("devtools.runtime.command", return_value="tests passed") as command,
        ):
            native_check()
            build.assert_called_once_with(tests=True)
            command.assert_called_once()
            invocation = command.call_args.args
            self.assertEqual(invocation[invocation.index("--entrypoint") + 1], "ctest")
            self.assertIn("--read-only", invocation)
            self.assertIn("--tmpfs=/build/out/Testing:rw,mode=1777,size=32m", invocation)
            self.assertEqual(invocation[-3:], ("--test-dir", "/build/out", "--output-on-failure"))


class GeneratorImageTests(unittest.TestCase):
    def test_source_fingerprint_changes_for_every_generator_build_input(self) -> None:
        with (
            tempfile.TemporaryDirectory() as directory,
            patch("devtools.runtime.ROOT", Path(directory)),
        ):
            root = Path(directory) / "src"
            names = (
                "load/main.cpp",
                "CMakeLists.txt",
                "Dockerfile",
                ".dockerignore",
                "cmake/dependencies.cmake",
                "cmake/targets.cmake",
                "tests/CMakeLists.txt",
                "benchmarks/CMakeLists.txt",
            )
            for name in names:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name, encoding="utf-8")
            original = generator_fingerprint()
            for name in names:
                with self.subTest(name=name):
                    path = root / name
                    path.write_text(name + " changed", encoding="utf-8")
                    self.assertNotEqual(generator_fingerprint(), original)
                    path.write_text(name, encoding="utf-8")

    def test_matching_generator_fingerprint_skips_rebuild(self) -> None:
        with (
            patch("devtools.runtime.generator_fingerprint", return_value="current"),
            patch("devtools.runtime.command", return_value="current") as command,
        ):
            ensure_generator()
            command.assert_called_once()
            self.assertEqual(command.call_args.args[:3], ("docker", "image", "inspect"))
            self.assertEqual(command.call_args.args[-1], GENERATOR_IMAGE)

    def test_missing_or_stale_generator_rebuilds_only_the_generator_target(self) -> None:
        for cached in ("old", None):
            with (
                self.subTest(cached=cached),
                patch("devtools.runtime.generator_fingerprint", return_value="current"),
                patch(
                    "devtools.runtime.command",
                    side_effect=[
                        cached
                        if cached is not None
                        else subprocess.CalledProcessError(1, "inspect"),
                        "",
                    ],
                ) as command,
            ):
                ensure_generator()
                self.assertEqual(command.call_count, 2)
                build = command.call_args.args
                self.assertEqual(build[:2], ("docker", "build"))
                self.assertEqual(build[build.index("--tag") + 1], GENERATOR_IMAGE)
                self.assertEqual(build[build.index("--target") + 1], "generator")
                self.assertNotIn("TDS_BUILD_TARGET=load_generator", build)

    def test_server_choice_cannot_select_an_obsolete_native_generator(self) -> None:
        captured: list[LoadGenerator] = []

        def loaded(
            _args: RunConfig, generator: LoadGenerator, _observer: str, _output: Path
        ) -> int:
            captured.append(generator)
            return 0

        variants: tuple[tuple[str, reports.TelemetryMode], ...] = (
            ("twelve-dollar-cpp:obsolete", "off"),
            ("twelve-dollar-cpp:local-basic", "basic"),
            ("twelve-dollar-cpp:local-profile", "profile"),
        )
        for image, telemetry in variants:
            with (
                self.subTest(image=image),
                tempfile.TemporaryDirectory() as directory,
                patch("devtools.runtime.ensure_generator") as ensure,
                patch("devtools.runtime.server") as server_context,
                patch("devtools.runtime.observing") as observer,
                patch("devtools.runtime.load", side_effect=loaded),
                patch(
                    "devtools.runtime.command",
                    side_effect=["127.0.0.1:6565", "current\nsha /app/load"],
                ) as command,
                patch(
                    "devtools.runtime.subprocess.run",
                    return_value=subprocess.CompletedProcess([], 0, "[]", ""),
                ),
            ):

                def entered_server() -> tuple[str, int]:
                    ensure.assert_called_once()
                    return "test-server", 3000

                server_context.return_value.__enter__.side_effect = entered_server
                observer.return_value.__enter__.return_value = "observer"
                output = Path(directory)
                write_json(output / "environment.json", {"docker_info": {"MemTotal": 24 * 2**30}})
                args = RunConfig(
                    label="unit-test",
                    mode="bench",
                    generator="cpp",
                    k6=None,
                    vus=1,
                    hold="60s",
                    ramp="30s",
                    telemetry=telemetry,
                )
                self.assertEqual(run_workload(args, output, image=image), 0)
                server_context.assert_called_once_with(output, image=image, telemetry=telemetry)
                self.assertIn(GENERATOR_IMAGE, captured[-1].invocation)
                self.assertNotIn(image, captured[-1].invocation)
                self.assertEqual("--diagnostics" in captured[-1].invocation, telemetry == "profile")
                self.assertIn(GENERATOR_IMAGE, command.call_args.args)

    def test_failed_generator_refresh_never_starts_the_server(self) -> None:
        with (
            patch(
                "devtools.runtime.ensure_generator",
                side_effect=subprocess.CalledProcessError(1, "build"),
            ),
            patch("devtools.runtime.server") as server_context,
        ):
            args = RunConfig(
                label="unit-test",
                vus=1,
                ramp="30s",
                hold="60s",
                mode="bench",
                generator="cpp",
                k6=None,
            )
            with self.assertRaises(subprocess.CalledProcessError):
                run_workload(args, Path("unused"), image="old-server")
            server_context.assert_not_called()

    def test_k6_does_not_require_the_native_generator_image(self) -> None:
        with (
            patch("devtools.runtime.ensure_generator") as ensure,
            patch("devtools.runtime.server") as server_context,
            patch("devtools.runtime.observing"),
            patch("devtools.runtime.load_generator"),
            patch("devtools.runtime.load", return_value=0),
        ):
            server_context.return_value.__enter__.return_value = ("test-server", 3000)
            args = RunConfig(
                label="unit-test",
                vus=1,
                ramp="30s",
                hold="60s",
                mode="bench",
                generator="k6",
                k6=None,
                telemetry="off",
            )
            self.assertEqual(run_workload(args, Path("unused"), image="server"), 0)
            ensure.assert_not_called()


class ToolBoundaryTests(unittest.TestCase):
    def test_parser_creates_an_immutable_workload_snapshot_without_cli_options(self) -> None:
        for mode, expected_vus in (("bench", 350000), ("profile", 5000)):
            with self.subTest(mode=mode):
                args = dev.build_parser().parse_args(
                    [mode, "--label", "snapshot"], namespace=dev.Arguments()
                )
                config = dev.workload_config(args)
                self.assertIsInstance(config, RunConfig)
                self.assertEqual(config.mode, mode)
                self.assertEqual(config.vus, expected_vus)
                self.assertEqual(config.telemetry, "off")
                self.assertEqual(
                    (config.ramp, config.hold, config.generator), ("30s", "60s", "cpp")
                )
                args.vus = 7
                self.assertEqual(config.vus, expected_vus)
                with self.assertRaises(FrozenInstanceError):
                    config.__setattr__("vus", 7)
                self.assertFalse(
                    any(hasattr(config, name) for name in ("save", "no_build", "image", "docker"))
                )

    def test_invalid_cli_workload_preserves_exit_codes_without_starting_resources(self) -> None:
        for options, exit_code in ((["--vus", "0"], 2), (["--hold", "0s"], 1)):
            with (
                self.subTest(options=options),
                patch("sys.argv", ["dev.py", "bench", "--label", "invalid", *options]),
                patch("sys.stderr", StringIO()),
                patch("devtools.runtime.run_workload") as run,
            ):
                with self.assertRaises(SystemExit) as stopped:
                    dev.main()
                self.assertEqual(stopped.exception.code, exit_code)
                run.assert_not_called()

    def test_cli_reuses_the_report_summary_for_diagnostics(self) -> None:
        metadata: dict[str, JSONValue] = {"seed": 42, "workflow_errors": 0}

        def completed(config: RunConfig, output: Path, *, image: str) -> int:
            self.assertEqual((config.vus, config.telemetry, image), (7, "profile", "old-runtime"))
            write_json(
                output / "run.json",
                {
                    "arguments": ["VUS=7", "DURATION=60s", "RAMP_UP=30s"],
                    "exit_code": 0,
                    "cgroup_before": {"usage_usec": 100},
                    "cgroup_after": {"usage_usec": 120, "memory_peak": 4096, "oom_kill": 0},
                    "cpu_scope": "server_cgroup",
                    "load_generator": "docker_loopback",
                    "generator": "cpp",
                },
            )
            write_json(
                output / "k6-summary.json",
                {
                    "metrics": {
                        "http_reqs": {"count": 2, "rate": 1},
                        "http_req_duration": {"p(95)": 1, "p(99)": 1, "max": 2},
                        "http_req_failed": {"value": 0},
                        "checks": {"fails": 0},
                    },
                    "generator": metadata,
                },
            )
            write_json(
                output / "environment.json", {"runtime": {"version": "0.1.0", "sqlite": "3.53.1"}}
            )
            write_json(output / "samples.json", [])
            write_json(output / "telemetry.json", parse_telemetry("", expected_mode="profile"))
            return 0

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            destination = root / "saved.json"
            with (
                patch("devtools.runtime.ROOT", root),
                patch("devtools.runtime.run_workload", side_effect=completed),
                patch("devtools.reports.read_json", wraps=reports.read_json) as read,
                patch("devtools.reports.show_results"),
                patch("devtools.reports.show_diagnostics") as show,
                patch("builtins.print"),
                patch(
                    "sys.argv",
                    [
                        "dev.py",
                        "bench",
                        "--label",
                        "single-read",
                        "--vus",
                        "7",
                        "--telemetry",
                        "profile",
                        "--image",
                        "old-runtime",
                        "--save",
                        str(destination),
                    ],
                ),
            ):
                with self.assertRaises(SystemExit) as stopped:
                    dev.main()
                self.assertEqual(stopped.exception.code, 0)
                summary = root / "results" / "single-read" / "k6-summary.json"
                self.assertEqual(sum(call.args == (summary,) for call in read.call_args_list), 1)
                show.assert_called_once_with(metadata)
            saved = parse_results(read_json(destination))
            self.assertEqual(saved[0]["cpu_scope"], "server_cgroup")
            self.assertEqual(saved[0]["cpu_us_per_request"], 10)

    def test_workload_failure_unwinds_generator_observer_and_server(self) -> None:
        cleanup: list[str] = []
        with (
            patch("devtools.runtime.ensure_generator"),
            patch("devtools.runtime.server") as server_context,
            patch("devtools.runtime.observing") as observer_context,
            patch("devtools.runtime.load_generator") as generator_context,
            patch("devtools.runtime.load", side_effect=RuntimeError("failed workload")),
        ):
            server_context.return_value.__enter__.return_value = ("server", 3000)
            for name, context in (
                ("server", server_context),
                ("observer", observer_context),
                ("generator", generator_context),
            ):

                def finished(
                    _exception_type: type[BaseException] | None,
                    _exception: BaseException | None,
                    _traceback: TracebackType | None,
                    *,
                    owner: str = name,
                ) -> bool:
                    cleanup.append(owner)
                    return False

                context.return_value.__exit__.side_effect = finished
            config = RunConfig(label="cleanup", vus=1, ramp="30s", hold="60s")
            with self.assertRaisesRegex(RuntimeError, "failed workload"):
                runtime.run_workload(config, Path("unused"), image="server")
        self.assertEqual(cleanup, ["generator", "observer", "server"])


def telemetry_record(source: str, **overrides: JSONValue) -> str:
    record: dict[str, JSONValue] = {
        "schema_version": 1,
        "source": source,
        "mode": "profile",
        "totals": {"requests": 2**64 - 1},
        "windows": [],
        "overwritten_windows": 0,
        "dropped_events": 0,
    }
    record.update(overrides)
    return json.dumps(record) + "\n"


class TelemetryTests(unittest.TestCase):
    def test_compact_telemetry_and_pretty_summary_preserve_json_values(self) -> None:
        value = {"counter": 2**64 - 1, "label": 'quote" and newline\n'}
        with tempfile.TemporaryDirectory() as directory:
            compact = Path(directory) / "telemetry.json"
            pretty = Path(directory) / "summary.json"
            write_json(compact, value, compact=True)
            write_json(pretty, value)
            self.assertEqual(read_json(compact), value)
            self.assertEqual(read_json(pretty), value)
            self.assertEqual(compact.read_text(encoding="utf-8").count("\n"), 1)
            self.assertIn('\n  "counter":', pretty.read_text(encoding="utf-8"))

    def test_profile_summary_keeps_diagnostics_without_copying_raw_histograms(self) -> None:
        windows: list[JSONValue] = [
            {
                "start_s": index * 5,
                "end_s": index * 5 + 5,
                "request_duration_ms": {
                    "count": 10,
                    "max_ms": 100 if index == 0 else index,
                    "p99_upper_ms": index,
                    "histogram": [[0, 1, 10]],
                },
            }
            for index in range(9)
        ]
        workload: dict[str, JSONValue] = {
            "seed": 42,
            "endpoint_requests": {"feed": 90},
            "diagnostics": {"windows_retained": 9, "windows": windows},
        }
        before = json.dumps(workload)
        summary = reports.workload_summary(workload)
        selected = mapping(summary["diagnostics"])["windows"]
        self.assertIsInstance(selected, list)
        assert isinstance(selected, list)
        self.assertEqual(len(selected), 6)
        self.assertIn(0, [mapping(value)["start_s"] for value in selected])
        self.assertEqual(summary["endpoint_requests"], {"feed": 90})
        self.assertNotIn('"histogram"', json.dumps(summary))
        self.assertEqual(json.dumps(workload), before)

    def test_telemetry_summary_keeps_metrics_loss_and_collection_scope(self) -> None:
        document = parse_telemetry(telemetry_record("database", dropped_events=6))
        owner = document["reports"][0]
        owner["totals"] = {
            "counters": {"commits": 3},
            "metrics": {
                "write_commit_ns": {
                    "unit": "ns",
                    "count": 3,
                    "total": 100,
                    "max": 50,
                    "buckets": [[5, 3]],
                },
                "unused": {"unit": "ns", "count": 0, "total": 0, "max": 0, "buckets": []},
            },
        }
        before = json.dumps(document)
        summary = reports.telemetry_summary(decode_json(json.dumps(document).encode()))
        self.assertFalse(summary["complete"])
        self.assertEqual(summary["scope"], "server_lifetime")
        self.assertEqual(summary["dropped_events"], 6)
        owners = summary["owners"]
        assert isinstance(owners, list)
        totals = mapping(owners[0])
        metrics = mapping(totals["metrics"])
        self.assertEqual(mapping(metrics["write_commit_ns"])["mean"], 33.333)
        self.assertNotIn("unused", metrics)
        self.assertNotIn('"buckets"', json.dumps(summary))
        self.assertEqual(json.dumps(document), before)

    def test_complete_export_keeps_exact_counters_and_owner_identity(self) -> None:
        report = parse_telemetry(
            "".join(telemetry_record(source) for source in reports.TELEMETRY_SOURCES),
            expected_mode="profile",
        )
        self.assertTrue(report["complete"])
        self.assertEqual(report["missing_sources"], [])
        self.assertEqual(report["mode"], "profile")
        self.assertEqual(mapping(report["reports"][0]["totals"])["requests"], 2**64 - 1)

    def test_missing_or_dropped_data_stays_visible(self) -> None:
        partial = parse_telemetry(telemetry_record("transport"))
        self.assertFalse(partial["complete"])
        self.assertEqual(partial["missing_sources"], ["database", "checkpoint"])
        bounded = parse_telemetry(
            telemetry_record("transport", overwritten_windows=5)
            + telemetry_record("database", dropped_events=3)
            + telemetry_record("checkpoint")
        )
        self.assertFalse(bounded["complete"])
        self.assertEqual(bounded["missing_sources"], [])
        self.assertEqual((bounded["overwritten_windows"], bounded["dropped_events"]), (5, 3))

    def test_invalid_or_truncated_exports_are_rejected(self) -> None:
        invalid = [
            "Listening\n",
            telemetry_record("transport") + '{"schema_version":1',
            telemetry_record("transport") * 2,
            telemetry_record("transport") + telemetry_record("database", mode="basic"),
            telemetry_record("unknown"),
            telemetry_record("transport", mode="off"),
            telemetry_record("transport", schema_version=True),
            telemetry_record("transport", schema_version=1.0),
            telemetry_record("transport", windows={}),
            telemetry_record("transport", dropped_events=-1),
            telemetry_record("transport", overwritten_windows=True),
            telemetry_record("transport", dropped_events=1.0),
            telemetry_record("transport", totals={"requests": float("nan")}),
        ]
        for data in invalid:
            with self.subTest(data=data), self.assertRaises(ValueError):
                parse_telemetry(data)

    def test_missing_loss_counters_never_declare_a_complete_export(self) -> None:
        for key in ("overwritten_windows", "dropped_events"):
            record = mapping(decode_json(telemetry_record("transport").encode()))
            del record[key]
            data = (
                json.dumps(record)
                + "\n"
                + telemetry_record("database")
                + telemetry_record("checkpoint")
            )
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "telemetry line 1"):
                parse_telemetry(data)
        with self.assertRaisesRegex(ValueError, "unexpected telemetry mode"):
            parse_telemetry(telemetry_record("transport", mode="basic"), expected_mode="profile")

    def test_build_modes_are_explicit_and_perf_stays_independent(self) -> None:
        modes: tuple[reports.TelemetryMode, ...] = ("off", "basic", "profile")
        for mode in modes:
            with self.subTest(mode=mode), patch("devtools.runtime.command") as command:
                image = runtime.build(telemetry=mode)
                self.assertEqual(image, runtime.IMAGE + ("" if mode == "off" else f"-{mode}"))
                self.assertIn(f"TDS_TELEMETRY={mode}", command.call_args.args)
                self.assertNotIn("TDS_DIAGNOSTICS=ON", command.call_args.args)
        with (
            patch("devtools.runtime.server") as server_context,
            patch("devtools.runtime.observing") as observer,
            patch("devtools.runtime.load_generator"),
            patch("devtools.runtime.load", return_value=0),
        ):
            server_context.return_value.__enter__.return_value = ("server", 3000)
            config = RunConfig(
                label="profile-off", vus=1, ramp="30s", hold="60s", mode="profile", generator="k6"
            )
            runtime.run_workload(config, Path("unused"), image=runtime.IMAGE)
            self.assertTrue(observer.call_args.kwargs["profiling"])
            self.assertEqual(observer.call_args.kwargs["profiler_image"], runtime.PROFILER_IMAGE)
            server_context.assert_called_once_with(
                Path("unused"), image=runtime.IMAGE, telemetry="off"
            )

    def test_export_is_copied_after_stop_and_before_container_removal(self) -> None:
        events: list[str] = []

        def command(*args: str, timeout: int | None = 30) -> str:
            del timeout
            if args[:2] == ("docker", "port"):
                return "127.0.0.1:12345"
            if args[-1] == "--version":
                return '{"telemetry":"profile"}'
            return "{}"

        def run(args: list[str], **_options: object) -> subprocess.CompletedProcess[str]:
            events.append(args[1])
            if args[1] == "cp":
                Path(args[-1]).write_text(
                    "".join(telemetry_record(source) for source in reports.TELEMETRY_SOURCES),
                    encoding="utf-8",
                )
            return subprocess.CompletedProcess(args, 0, "[]", "")

        with (
            tempfile.TemporaryDirectory() as directory,
            patch("devtools.runtime.command", side_effect=command),
            patch("devtools.runtime.healthy", return_value=True),
            patch("devtools.runtime.subprocess.run", side_effect=run),
        ):
            output = Path(directory)
            seed = output / "fixture.db"
            seed.touch()
            with server(output, database=seed, telemetry="profile"):
                pass
            self.assertLess(events.index("stop"), events.index("cp"))
            self.assertLess(events.index("cp"), events.index("rm"))
            saved = mapping(read_json(output / "telemetry.json"))
            self.assertTrue(saved["complete"])
            self.assertTrue((output / "telemetry.jsonl").is_file())

    def test_missing_export_does_not_mask_workload_failure_or_cleanup(self) -> None:
        def command(*args: str, timeout: int | None = 30) -> str:
            del timeout
            if args[:2] == ("docker", "port"):
                return "127.0.0.1:12345"
            return '{"telemetry":"basic"}' if args[-1] == "--version" else "{}"

        def run(args: list[str], **_options: object) -> subprocess.CompletedProcess[str]:
            return subprocess.CompletedProcess(
                args, 1 if args[1] == "cp" else 0, "[]", "export missing" if args[1] == "cp" else ""
            )

        with (
            tempfile.TemporaryDirectory() as directory,
            patch("devtools.runtime.command", side_effect=command) as commands,
            patch("devtools.runtime.healthy", return_value=True),
            patch("devtools.runtime.subprocess.run", side_effect=run) as runs,
            patch("builtins.print"),
        ):
            output = Path(directory)
            seed = output / "fixture.db"
            seed.touch()
            container = ""
            with self.assertRaisesRegex(RuntimeError, "workload failed"):
                with server(output, database=seed, telemetry="basic") as (name, _port):
                    container = name
                    raise RuntimeError("workload failed")
            self.assertFalse(mapping(read_json(output / "telemetry.json"))["complete"])
            runs.assert_any_call(
                ["docker", "rm", "--force", container], capture_output=True, check=False
            )
            commands.assert_any_call("docker", "volume", "rm", container + "-data")

    def test_wrong_binary_mode_is_rejected_before_measuring(self) -> None:
        def command(*args: str, timeout: int | None = 30) -> str:
            del timeout
            if args[:2] == ("docker", "port"):
                return "127.0.0.1:12345"
            return '{"telemetry":"profile"}' if args[-1] == "--version" else "{}"

        with (
            tempfile.TemporaryDirectory() as directory,
            patch("devtools.runtime.command", side_effect=command),
            patch("devtools.runtime.healthy", return_value=True),
            patch(
                "devtools.runtime.subprocess.run",
                return_value=subprocess.CompletedProcess([], 0, "[]", ""),
            ),
        ):
            output = Path(directory)
            seed = output / "fixture.db"
            seed.touch()
            entered = False
            with self.assertRaisesRegex(RuntimeError, "requested 'off'"):
                with server(output, database=seed, telemetry="off"):
                    entered = True
            self.assertFalse(entered)

    def test_truncated_artifact_stays_raw_and_collection_is_unsuccessful(self) -> None:
        def copied(args: list[str], **_options: object) -> subprocess.CompletedProcess[str]:
            Path(args[-1]).write_text(
                telemetry_record("transport") + '{"source":', encoding="utf-8"
            )
            return subprocess.CompletedProcess(args, 0, "", "")

        with (
            tempfile.TemporaryDirectory() as directory,
            patch("devtools.runtime.subprocess.run", side_effect=copied),
            patch("builtins.print") as output_message,
        ):
            output = Path(directory)
            runtime.collect_telemetry("stopped-server", output, "profile")
            report = mapping(read_json(output / "telemetry.json"))
            self.assertFalse(report["complete"])
            self.assertTrue(report["collection_errors"])
            self.assertTrue((output / "telemetry.jsonl").read_text().endswith('{"source":'))
            output_message.assert_called_once()
