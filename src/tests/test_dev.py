"""Tests for dev tooling, workload orchestration and profile analysis"""

import hashlib
import json
import subprocess
import tempfile
import unittest
from collections.abc import Callable
from dataclasses import FrozenInstanceError
from io import StringIO
from pathlib import Path
from types import TracebackType
from unittest.mock import patch

import dev
from devtools import images, reports, runtime
from devtools.checks import native_check
from devtools.images import GENERATOR_IMAGE, ensure_generator, generator_fingerprint
from devtools.reports import (
    JSONValue,
    Result,
    analyze_profile,
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
    LoadGenerator,
    RunConfig,
    load,
    load_generator,
    run_workload,
    server,
)
from devtools.sources import SourceSnapshot
from tests import TemporaryDirectory


class TemporaryCleanupTests(unittest.TestCase):
    def test_windows_delayed_unlink_is_retried(self) -> None:
        temporary = TemporaryDirectory()
        try:
            busy = OSError(145, "directory not empty", None, 145)
            with (
                patch.object(
                    tempfile.TemporaryDirectory, "cleanup", side_effect=[busy, None]
                ) as cleanup,
                patch("tests.time.sleep") as sleep,
            ):
                temporary.cleanup()
                self.assertEqual(cleanup.call_count, 2)
                sleep.assert_called_once_with(0.05)
        finally:
            temporary.cleanup()

    def test_retries_are_bounded_and_other_errors_propagate(self) -> None:
        cases = (
            (OSError(145, "directory not empty", None, 145), 5),
            (PermissionError(5, "denied", None, 5), 1),
            (OSError("other error"), 1),
        )
        for error, attempts in cases:
            with self.subTest(error=error):
                temporary = TemporaryDirectory()
                try:
                    with (
                        patch.object(
                            tempfile.TemporaryDirectory, "cleanup", side_effect=error
                        ) as cleanup,
                        patch("tests.time.sleep") as sleep,
                    ):
                        with self.assertRaises(OSError) as raised:
                            temporary.cleanup()
                        self.assertIs(raised.exception, error)
                        self.assertEqual(cleanup.call_count, attempts)
                        self.assertEqual(sleep.call_count, attempts - 1)
                finally:
                    temporary.cleanup()


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
                TemporaryDirectory() as directory,
                patch(
                    "devtools.images.image_command",
                    return_value="cpp-version",
                ) as version,
                patch("devtools.images.inspect") as inspect,
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
                    vus=1,
                    hold="60s",
                    ramp="30s",
                    seed=seed,
                )
                with load_generator(
                    args,
                    "test-container",
                    output,
                    image=make_identity(GENERATOR_IMAGE, "generator-hash"),
                    workspace=runtime.ROOT,
                ) as generator:
                    self.assertIn(GENERATOR_IMAGE, generator.invocation)
                    self.assertEqual(generator.sha256, "generator-hash")
                    version.assert_called_once_with(
                        make_identity(GENERATOR_IMAGE, "generator-hash"),
                        "/app/load_generator",
                        "--version",
                    )
                    inspect.assert_not_called()
                    if seed is None:
                        self.assertNotIn("--seed", generator.invocation)
                    else:
                        index = generator.invocation.index("--seed")
                        self.assertEqual(generator.invocation[index + 1], str(seed))

    def test_legacy_result_reader_preserves_saved_experiment_bytes(self) -> None:
        with TemporaryDirectory() as directory:
            path = Path(directory) / "results.json"
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
            write_json(path, {"runs": [result], "experiment": {"hold": [1, 2]}})
            original = path.read_bytes()
            self.assertEqual(parse_results(reports.load_saved_report(path)), [result])
            self.assertEqual(path.read_bytes(), original)
            self.assertEqual(mapping(read_json(path))["experiment"], {"hold": [1, 2]})
            write_json(path, {"invalid": "root"})
            with self.assertRaisesRegex(ValueError, "JSON array"):
                parse_results(reports.load_saved_report(path))
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
        args = RunConfig(vus=1, label="exit-race", ramp="30s", hold="60s")
        generator = LoadGenerator(["k6"], 6565, "test", "test", "load-container")
        snapshot: dict[str, JSONValue] = {
            "user_usec": 0,
            "system_usec": 0,
            "main_user_ticks": 0,
            "main_system_ticks": 0,
        }
        with (
            TemporaryDirectory() as directory,
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
        args = RunConfig(vus=1, label="native-gate", ramp="30s", hold="60s")
        generator = LoadGenerator(["load_generator"], 0, "test", "test", "native-load", kind="cpp")
        snapshot: dict[str, JSONValue] = {
            "user_usec": 0,
            "system_usec": 0,
            "main_user_ticks": 0,
            "main_system_ticks": 0,
        }
        with (
            TemporaryDirectory() as directory,
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
            self.assertEqual(mapping(read_json(output / "run.json"))["workload"], args.workload)

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
            if args[:2] == ("docker", "port"):
                return "127.0.0.1:12345"
            return '[{"Image":"server-pinned"}]' if args[:2] == ("docker", "inspect") else "{}"

        with (
            TemporaryDirectory() as directory,
            patch("devtools.runtime.command", side_effect=response) as command,
            patch("devtools.runtime.healthy", return_value=True),
            patch("devtools.runtime.subprocess.run") as run,
            patch("builtins.print"),
        ):
            output = Path(directory)
            seed = output / "fixture.db"
            seed.touch()
            workspace = output / "frozen"
            (workspace / "bench").mkdir(parents=True)
            (workspace / "bench" / "load.js").write_bytes(b"frozen workload")
            run.side_effect = [
                subprocess.TimeoutExpired("docker stop", 20),
                subprocess.CompletedProcess(["docker", "inspect"], 0, "[]", ""),
                subprocess.CompletedProcess(["docker", "logs"], 0, "Listening\n", ""),
                subprocess.CompletedProcess(["docker", "rm"], 0, "", ""),
            ]
            container_name = ""
            with self.assertRaisesRegex(RuntimeError, "request failed"):
                with server(output, image=make_identity(), database=seed, workspace=workspace) as (
                    name,
                    port,
                ):
                    container_name = name
                    self.assertEqual(port, 12345)
                    raise RuntimeError("request failed")
            run.assert_any_call(
                ["docker", "rm", "--force", container_name], capture_output=True, check=False
            )
            command.assert_any_call("docker", "volume", "rm", container_name + "-data")
            self.assertEqual((output / "server.log").read_text(), "Listening\n")
            launch = next(call.args for call in command.call_args_list if "--detach" in call.args)
            self.assertIn(f"--security-opt=seccomp={workspace / 'src/docker/seccomp.json'}", launch)
            environment = mapping(read_json(output / "environment.json"))
            self.assertEqual(
                environment["workload_sha256"], hashlib.sha256(b"frozen workload").hexdigest()
            )

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
            patch("devtools.images.build") as build,
            patch("devtools.checks.cpp_editor"),
            patch("devtools.runtime.command", return_value="tests passed") as command,
        ):
            native_check("native-tests")
            build.assert_not_called()
            command.assert_called_once()
            invocation = command.call_args.args
            self.assertIn("--entrypoint=ctest", invocation)
            self.assertIn("--read-only", invocation)
            self.assertIn("--tmpfs=/build/out/Testing:rw,mode=1777,size=32m", invocation)
            self.assertEqual(invocation[-3:], ("--test-dir", "/build/out", "--output-on-failure"))


class GeneratorImageTests(unittest.TestCase):
    def test_generator_build_records_only_effective_settings(self) -> None:
        with (
            patch("devtools.images._command") as command,
            patch("devtools.images.inspect", return_value=make_identity()),
            patch("devtools.images.generator_fingerprint", return_value="generator-source"),
            patch(
                "devtools.runtime.subprocess.run",
                return_value=subprocess.CompletedProcess([], 0, "", ""),
            ),
        ):
            images.build(make_snapshot(runtime.ROOT), "generator", telemetry="profile")
            invocation = command.call_args_list[0].args
            spec_label = next(
                value
                for value in invocation
                if value.startswith("--label=" + images.BUILD_LABEL + "=")
            )
            spec = mapping(decode_json(spec_label.split("=", 2)[2].encode()))
            self.assertEqual(spec["target"], "generator")
            self.assertTrue(spec["generator_only"])
            self.assertNotIn("telemetry", spec)
            self.assertFalse(
                any(value.startswith("--build-arg=TDS_TELEMETRY=") for value in invocation)
            )

    def test_source_fingerprint_changes_for_every_generator_build_input(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory) / "src"
            names = (
                "load/main.cpp",
                "load/detail/settings.hpp",
                "load/CMakeLists.txt",
                "CMakeLists.txt",
                "Dockerfile",
                ".dockerignore",
                "cmake/dependencies.cmake",
                "cmake/targets.cmake",
            )
            for name in names:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name, encoding="utf-8")
            original = generator_fingerprint(root)
            for name in names:
                with self.subTest(name=name):
                    path = root / name
                    path.write_text(name + " changed", encoding="utf-8")
                    self.assertNotEqual(generator_fingerprint(root), original)
                    path.write_text(name, encoding="utf-8")
            for name in (
                "tests/CMakeLists.txt",
                "benchmarks/CMakeLists.txt",
                "cmake/server-dependencies.cmake",
            ):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("unrelated", encoding="utf-8")
                self.assertEqual(generator_fingerprint(root), original)

    def test_matching_generator_fingerprint_skips_rebuild(self) -> None:
        inspected = {
            "Id": "generator-pinned",
            "Config": {"Labels": {images.GENERATOR_SOURCE_LABEL: "current"}},
            "RepoDigests": ["generator@sha256:pinned"],
        }
        with (
            patch("devtools.images.generator_fingerprint", return_value="current"),
            patch(
                "devtools.images._command",
                side_effect=[
                    json.dumps(inspected),
                    "toolchain",
                    "generator-hash /app/load_generator",
                ],
            ) as command,
            patch("devtools.images.build") as build,
        ):
            identity = ensure_generator(make_snapshot(runtime.ROOT))
            self.assertEqual(identity["image_id"], "generator-pinned")
            self.assertEqual(identity["binary_sha256"], "generator-hash")
            self.assertEqual(identity["repo_digests"], inspected["RepoDigests"])
            inspections = [
                call
                for call in command.call_args_list
                if call.args[:3] == ("docker", "image", "inspect")
            ]
            self.assertEqual(len(inspections), 1)
            self.assertEqual(inspections[0].args[-1], GENERATOR_IMAGE)
            for call in command.call_args_list[1:]:
                self.assertIn("generator-pinned", call.args)
                self.assertNotIn(GENERATOR_IMAGE, call.args)
            build.assert_not_called()

    def test_missing_or_stale_generator_rebuilds_only_the_generator_target(self) -> None:
        for cached in ("old", None):
            with (
                self.subTest(cached=cached),
                patch("devtools.images.generator_fingerprint", return_value="current"),
                patch(
                    "devtools.images._command",
                    side_effect=[
                        json.dumps({"Config": {"Labels": {images.GENERATOR_SOURCE_LABEL: cached}}})
                        if cached is not None
                        else subprocess.CalledProcessError(1, "inspect"),
                    ],
                ) as command,
                patch(
                    "devtools.images.build", return_value=make_identity("generator-pinned")
                ) as build,
            ):
                source = make_snapshot(runtime.ROOT)
                self.assertEqual(ensure_generator(source)["image_id"], "generator-pinned")
                command.assert_called_once()
                build.assert_called_once_with(source, "generator")

    def test_server_choice_cannot_select_an_obsolete_native_generator(self) -> None:
        captured: list[LoadGenerator] = []

        def loaded(
            _args: RunConfig, generator: LoadGenerator, _observer: str, _output: Path
        ) -> int:
            captured.append(generator)
            return 0

        variants: tuple[tuple[str, reports.TelemetryMode, bool], ...] = (
            ("twelve-dollar-cpp:obsolete", "off", False),
            ("twelve-dollar-cpp:local-basic", "basic", True),
            ("twelve-dollar-cpp:local-profile", "profile", False),
        )
        for image_name, telemetry, diagnostics in variants:
            image = make_identity(image_name)
            source = make_snapshot(runtime.ROOT)
            generator_identity = make_identity("generator-pinned", "generator-hash")
            with (
                self.subTest(image=image_name),
                TemporaryDirectory() as directory,
                patch(
                    "devtools.images.ensure_generator", return_value=generator_identity
                ) as ensure,
                patch("devtools.runtime.server") as server_context,
                patch("devtools.runtime.observing") as observer,
                patch("devtools.runtime.load", side_effect=loaded),
                patch(
                    "devtools.images.image_command",
                    return_value="current",
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
                    generator="cpp",
                    vus=1,
                    hold="60s",
                    ramp="30s",
                    telemetry=telemetry,
                    client_diagnostics=diagnostics,
                )
                self.assertEqual(run_workload(args, output, image=image, source=source), 0)
                server_context.assert_called_once_with(
                    output, image=image, telemetry=telemetry, workspace=runtime.ROOT
                )
                self.assertIn("generator-pinned", captured[-1].invocation)
                self.assertNotIn(image_name, captured[-1].invocation)
                self.assertEqual("--diagnostics" in captured[-1].invocation, diagnostics)
                self.assertIs(command.call_args.args[0], generator_identity)

    def test_failed_generator_refresh_never_starts_the_server(self) -> None:
        with (
            patch(
                "devtools.images.ensure_generator",
                side_effect=subprocess.CalledProcessError(1, "build"),
            ),
            patch("devtools.runtime.server") as server_context,
        ):
            args = RunConfig(
                label="unit-test",
                vus=1,
                ramp="30s",
                hold="60s",
                generator="cpp",
            )
            with self.assertRaises(subprocess.CalledProcessError):
                run_workload(
                    args,
                    Path("unused"),
                    image=make_identity("old-server"),
                    source=make_snapshot(runtime.ROOT),
                )
            server_context.assert_not_called()

    def test_k6_does_not_require_the_native_generator_image(self) -> None:
        with (
            patch("devtools.images.ensure_generator") as ensure,
            patch("devtools.images.inspect", return_value=make_identity("k6-pinned")) as inspect,
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
                generator="k6",
                telemetry="off",
            )
            self.assertEqual(
                run_workload(
                    args, Path("unused"), image=make_identity(), source=make_snapshot(runtime.ROOT)
                ),
                0,
            )
            ensure.assert_not_called()
            inspect.assert_called_once_with(images.K6_IMAGE, "/usr/bin/k6")


CLI_ANCHOR = "a" * 40
CLI_PATCH = b"captured source bytes\n"
CLI_SOURCE = "b" * 64


def make_identity(
    image_id: str = "server-pinned", binary_sha256: str = "e" * 64
) -> images.ImageIdentity:
    return images.ImageIdentity(
        image_id=image_id,
        binary_sha256=binary_sha256,
        repo_digests=[],
        source={
            "base_commit": CLI_ANCHOR,
            "patch_sha256": hashlib.sha256(CLI_PATCH).hexdigest(),
            "source_sha256": CLI_SOURCE,
        },
        build_spec={"telemetry": "off"},
        toolchain_sha256="f" * 64,
        build_sha256="c" * 64,
    )


def make_snapshot(root: Path) -> SourceSnapshot:
    return SourceSnapshot(
        root=root,
        context=root / "src",
        base_commit=CLI_ANCHOR,
        patch_sha256=hashlib.sha256(CLI_PATCH).hexdigest(),
        source_sha256=CLI_SOURCE,
        patch=CLI_PATCH,
        files=(),
    )


class VerifiedRuntimeTests(unittest.TestCase):
    def test_server_checks_running_image_without_reinspecting_prepared_metadata(self) -> None:
        identity = make_identity()
        for running_image in (identity["image_id"], "changed-server"):
            with (
                self.subTest(running_image=running_image),
                TemporaryDirectory() as directory,
                patch("devtools.images.inspect") as inspect,
                patch("devtools.images._command") as image_command,
                patch("devtools.runtime.healthy", return_value=True),
                patch(
                    "devtools.runtime.subprocess.run",
                    return_value=subprocess.CompletedProcess([], 0, "[]", ""),
                ),
                patch("builtins.print"),
            ):
                output = Path(directory)
                seed = output / "fixture.db"
                seed.touch()
                (output / "bench").mkdir()
                (output / "bench" / "load.js").write_bytes(b"frozen workload")

                def response(
                    *args: str, timeout: int | None = 30, actual: str = running_image
                ) -> str:
                    del timeout
                    if args[:2] == ("docker", "port"):
                        return "127.0.0.1:12345"
                    if args[:2] == ("docker", "inspect"):
                        return json.dumps([{"Image": actual}])
                    return '{"telemetry":"off"}' if args[-1] == "--version" else "{}"

                with patch("devtools.runtime.command", side_effect=response) as command:
                    if running_image == identity["image_id"]:
                        with server(output, image=identity, database=seed, workspace=output):
                            environment = mapping(read_json(output / "environment.json"))
                            self.assertEqual(environment["server_identity"], identity)
                    else:
                        with self.assertRaisesRegex(RuntimeError, "verified image"):
                            with server(output, image=identity, database=seed, workspace=output):
                                self.fail("a different running image cannot be measured")
                    self.assertTrue(
                        any(
                            call.args[:2] == ("docker", "inspect")
                            for call in command.call_args_list
                        )
                    )
                    self.assertFalse(
                        any(
                            "sha256sum" in call.args or "/etc/tds-toolchain" in call.args
                            for call in command.call_args_list
                        )
                    )
                inspect.assert_not_called()
                image_command.assert_not_called()

    def test_profiler_hash_mismatch_is_rejected_without_rehashing_binaries(self) -> None:
        with (
            patch("devtools.runtime.command") as command,
            patch("devtools.images.inspect") as inspect,
            patch("devtools.images._command") as image_command,
        ):
            with self.assertRaisesRegex(RuntimeError, "symbols do not match"):
                with runtime.observing(
                    "server",
                    Path("unused"),
                    image=make_identity(),
                    profiling=True,
                    profiler_image=make_identity("profiler-pinned", "different-binary"),
                ):
                    self.fail("mismatched symbols cannot enter the measurement")
            command.assert_not_called()
            inspect.assert_not_called()
            image_command.assert_not_called()

    def test_matching_profiler_metadata_uses_pinned_image_and_retains_exit_130(self) -> None:
        identity = make_identity()
        profiler = make_identity("profiler-pinned")

        def response(*args: str, timeout: int | None = 30) -> str:
            del timeout
            if "--format={{.State.Running}}" in args:
                return "true"
            if "--format={{.State.ExitCode}}" in args:
                return "130"
            return "[]" if args[:2] == ("docker", "inspect") else "perf version"

        with (
            TemporaryDirectory() as directory,
            patch("devtools.runtime.command", side_effect=response) as command,
            patch("devtools.runtime.perf_control"),
            patch("devtools.runtime.time.sleep"),
            patch("devtools.runtime.collect_profile") as collect,
            patch("devtools.images.inspect") as inspect,
            patch("devtools.images._command") as image_command,
            patch(
                "devtools.runtime.subprocess.run",
                return_value=subprocess.CompletedProcess([], 0, "perf log", ""),
            ),
        ):
            output = Path(directory)
            with runtime.observing(
                "server", output, image=identity, profiling=True, profiler_image=profiler
            ):
                sampler = mapping(read_json(output / "sampler.json"))
                self.assertEqual(sampler["binary_sha256"], identity["binary_sha256"])
            self.assertIn("profiler-pinned", command.call_args_list[0].args)
            self.assertFalse(any("sha256sum" in call.args for call in command.call_args_list))
            collect.assert_called_once_with("server", output, scope="user", image=profiler)
            inspect.assert_not_called()
            image_command.assert_not_called()


def canonical_path(root: Path, label: str) -> Path:
    return root / "src" / "runs" / CLI_ANCHOR[:7] / label


def write_cli_workload(config: RunConfig, output: Path, code: int = 0) -> int:
    write_json(
        output / "run.json",
        {
            "arguments": [f"VUS={config.vus}", f"DURATION={config.hold}", f"RAMP_UP={config.ramp}"],
            "workload": config.workload,
            "exit_code": code,
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
                "checks": {"fails": 0 if code == 0 else 1},
            },
            "generator": {"seed": 42, "workflow_errors": 0},
        },
    )
    write_json(output / "environment.json", {"runtime": {"version": "0.1.0", "sqlite": "3.53.1"}})
    write_json(output / "samples.json", [])
    if config.telemetry != "off":
        rows = "".join(
            telemetry_record(source, mode=config.telemetry)
            for source in ("transport", "database", "checkpoint")
        )
        (output / "telemetry.jsonl").write_text(rows, encoding="utf-8", newline="\n")
    return code


def invoke_cli(
    test: unittest.TestCase,
    root: Path,
    arguments: list[str],
    *,
    workload: Callable[[RunConfig, Path], int] | BaseException | None = None,
    known_source: bool = True,
    capture_error: BaseException | None = None,
    expected_targets: tuple[images.BuildTarget, ...] | None = None,
) -> int | str | None:
    snapshot = make_snapshot(root)
    identity: images.ImageIdentity = {
        "image_id": "sha256:" + "d" * 64,
        "binary_sha256": "e" * 64,
        "repo_digests": [],
        "build_spec": {"telemetry": "off"},
        "toolchain_sha256": "f" * 64,
        "build_sha256": "c" * 64,
        "source": {
            "base_commit": CLI_ANCHOR,
            "patch_sha256": snapshot.patch_sha256,
            "source_sha256": CLI_SOURCE,
        }
        if known_source
        else None,
    }

    def completed(
        config: RunConfig,
        output: Path,
        *,
        image: images.ImageIdentity,
        source: SourceSnapshot,
        profiler_image: images.ImageIdentity | None = None,
    ) -> int:
        test.assertIs(image, identity)
        test.assertIs(source, snapshot)
        test.assertEqual(output.name, "raw")
        if config.perf != "off":
            test.assertIs(profiler_image, identity)
        if isinstance(workload, BaseException):
            raise workload
        return (workload or write_cli_workload)(config, output)

    with (
        patch("devtools.runtime.ROOT", root),
        patch("devtools.runtime.command", return_value=CLI_ANCHOR),
        patch("devtools.sources.capture") as capture,
        patch("devtools.images.build", return_value=identity) as build,
        patch("devtools.images.inspect", return_value=identity) as inspect,
        patch("devtools.runtime.run_workload", side_effect=completed),
        patch("devtools.reports.show_saved_report"),
        patch("sys.argv", ["dev.py", *arguments]),
        patch("sys.stderr", StringIO()),
        patch("builtins.print"),
    ):
        capture.return_value.__enter__.return_value = snapshot
        capture.return_value.__enter__.side_effect = capture_error
        with test.assertRaises(SystemExit) as stopped:
            dev.main()
        if expected_targets is not None:
            test.assertEqual(
                tuple(
                    call.args[1] if len(call.args) > 1 else "final" for call in build.call_args_list
                ),
                expected_targets,
            )
        test.assertEqual(
            inspect.call_count, int("--image" in arguments or "--reuse-server" in arguments)
        )
        return stopped.exception.code


class CanonicalCliTests(unittest.TestCase):
    def test_reuse_server_skips_only_the_server_build(self) -> None:
        cases: tuple[tuple[list[str], tuple[images.BuildTarget, ...]], ...] = (
            ([], ("final",)),
            (["--reuse-server"], ()),
            (["--image", "reference"], ()),
            (["--reuse-server", "--perf", "user"], ("profiler",)),
        )
        with TemporaryDirectory() as directory:
            root = Path(directory)
            for index, (options, targets) in enumerate(cases):
                with self.subTest(options=options):
                    self.assertEqual(
                        invoke_cli(
                            self,
                            root,
                            ["bench", "--label", f"reuse-{index}", *options],
                            expected_targets=targets,
                        ),
                        0,
                    )

    def test_source_integrity_failure_cannot_return_success(self) -> None:
        def altered(config: RunConfig, output: Path) -> int:
            (output.parent / "source.patch").write_bytes(b"altered")
            return write_cli_workload(config, output)

        with TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(
                invoke_cli(self, root, ["bench", "--label", "altered"], workload=altered), 1
            )
            saved = mapping(read_json(canonical_path(root, "altered") / "summary.json"))
            self.assertEqual(saved["status"], "failed")

    def test_instruments_are_recorded_independently(self) -> None:
        telemetry_modes: tuple[reports.TelemetryMode, ...] = ("off", "basic", "profile")
        perf_modes: tuple[runtime.PerfMode, ...] = ("off", "user", "kernel", "all")
        with TemporaryDirectory() as directory:
            root = Path(directory)
            for telemetry in telemetry_modes:
                for diagnostics in (False, True):
                    for perf in perf_modes:
                        label = f"{telemetry}-{int(diagnostics)}-{perf}"
                        options = [
                            "bench",
                            "--label",
                            label,
                            "--telemetry",
                            telemetry,
                            "--perf",
                            perf,
                        ]
                        if diagnostics:
                            options.append("--client-diagnostics")
                        with self.subTest(telemetry=telemetry, diagnostics=diagnostics, perf=perf):
                            self.assertEqual(invoke_cli(self, root, options), 0)
                            manifest = mapping(
                                read_json(canonical_path(root, label) / "manifest.json")
                            )
                            self.assertEqual(
                                manifest["instruments"],
                                {
                                    "server_telemetry": telemetry,
                                    "client_diagnostics": diagnostics,
                                    "perf": perf,
                                },
                            )

    def test_nonzero_workload_keeps_exit_code_and_failed_benchmark(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory)
            code = invoke_cli(
                self,
                root,
                ["bench", "--label", "thresholds"],
                workload=lambda config, output: write_cli_workload(config, output, 99),
            )
            self.assertEqual(code, 99)
            saved = mapping(read_json(canonical_path(root, "thresholds") / "summary.json"))
            self.assertEqual((saved["status"], saved["exit_code"]), ("failed", 99))
            self.assertFalse(mapping(saved["benchmark"])["passed"])

    def test_nonzero_workload_is_preserved_when_cleanup_also_raises(self) -> None:
        def failed_cleanup(config: RunConfig, output: Path) -> int:
            write_cli_workload(config, output, 99)
            raise RuntimeError("server cleanup failed")

        with TemporaryDirectory() as directory:
            root = Path(directory)
            code = invoke_cli(
                self, root, ["bench", "--label", "threshold-cleanup"], workload=failed_cleanup
            )
            self.assertEqual(code, 99)
            saved = mapping(read_json(canonical_path(root, "threshold-cleanup") / "summary.json"))
            self.assertEqual(saved["error"], "load generator exited 99")
            self.assertEqual(saved["secondary_errors"], ["server cleanup failed"])

    def test_bounded_telemetry_loss_remains_visible_without_failing_http_workload(self) -> None:
        def bounded(config: RunConfig, output: Path) -> int:
            code = write_cli_workload(config, output)
            (output / "telemetry.jsonl").write_text(
                telemetry_record("transport", mode=config.telemetry, dropped_events=3)
                + telemetry_record("database", mode=config.telemetry)
                + telemetry_record("checkpoint", mode=config.telemetry),
                encoding="utf-8",
            )
            return code

        with TemporaryDirectory() as directory:
            root = Path(directory)
            code = invoke_cli(
                self,
                root,
                ["bench", "--label", "bounded", "--telemetry", "basic"],
                workload=bounded,
            )
            self.assertEqual(code, 0)
            saved = mapping(read_json(canonical_path(root, "bounded") / "summary.json"))
            self.assertEqual(saved["status"], "passed")
            telemetry = mapping(saved["server_telemetry"])
            self.assertFalse(telemetry["complete"])
            self.assertEqual(telemetry["dropped_events"], 3)

    def test_initialization_and_capture_failures_still_finish_without_benchmark(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory)
            for label, failure in (("initialize", "init"), ("capture", "capture")):
                with self.subTest(failure=failure):
                    code = invoke_cli(
                        self,
                        root,
                        ["bench", "--label", label],
                        workload=RuntimeError("initialization failed")
                        if failure == "init"
                        else None,
                        capture_error=RuntimeError("capture failed")
                        if failure == "capture"
                        else None,
                    )
                    self.assertEqual(code, 1)
                    saved = mapping(read_json(canonical_path(root, label) / "summary.json"))
                    self.assertEqual(saved["status"], "failed")
                    self.assertNotIn("benchmark", saved)
                    self.assertEqual(
                        saved["error"],
                        "initialization failed" if failure == "init" else "capture failed",
                    )

    def test_keyboard_interrupt_finishes_with_130(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(
                invoke_cli(
                    self, root, ["bench", "--label", "interrupt"], workload=KeyboardInterrupt()
                ),
                130,
            )
            saved = mapping(read_json(canonical_path(root, "interrupt") / "summary.json"))
            self.assertEqual((saved["status"], saved["error"]), ("interrupted", "interrupted"))

    def test_primary_failure_and_multiple_cleanup_failures_remain_distinct(self) -> None:
        error = RuntimeError("workload failed")
        error.add_note("server cleanup failed")
        error.add_note("generator cleanup failed")

        def failed(config: RunConfig, output: Path) -> int:
            write_cli_workload(config, output)
            write_json(
                output / "collection-errors.json",
                ["server cleanup failed", "generator cleanup failed"],
            )
            raise error

        with TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(
                invoke_cli(self, root, ["bench", "--label", "cleanup"], workload=failed), 1
            )
            saved = mapping(read_json(canonical_path(root, "cleanup") / "summary.json"))
            self.assertEqual(saved["error"], "workload failed")
            self.assertEqual(
                saved["secondary_errors"], ["server cleanup failed", "generator cleanup failed"]
            )

    def test_missing_requested_telemetry_fails_collection_without_changing_http_pass(self) -> None:
        def missing(config: RunConfig, output: Path) -> int:
            code = write_cli_workload(config, output)
            (output / "telemetry.jsonl").unlink()
            return code

        with TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(
                invoke_cli(
                    self,
                    root,
                    ["bench", "--label", "missing", "--telemetry", "basic"],
                    workload=missing,
                ),
                1,
            )
            saved = mapping(read_json(canonical_path(root, "missing") / "summary.json"))
            self.assertEqual(saved["status"], "failed")
            self.assertTrue(mapping(saved["benchmark"])["passed"])
            self.assertEqual(
                saved["secondary_errors"], ["requested server telemetry export is missing"]
            )

    def test_reused_unknown_image_records_binary_and_observer_without_claiming_source(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(
                invoke_cli(
                    self,
                    root,
                    ["bench", "--label", "reference", "--image", "historical"],
                    known_source=False,
                ),
                0,
            )
            path = canonical_path(root, "reference")
            provenance = mapping(mapping(read_json(path / "manifest.json"))["provenance"])
            self.assertIsNone(provenance["source"])
            self.assertFalse((path / "source.patch").exists())
            self.assertEqual((path / "raw" / "observer.patch").read_bytes(), CLI_PATCH)
            self.assertEqual(mapping(mapping(provenance["binary"])["server"])["sha256"], "e" * 64)

    def test_perf_analysis_stays_raw_when_final_summary_is_written(self) -> None:
        profile: dict[str, JSONValue] = {
            "event": "cpu-clock:u",
            "samples": 1,
            "sampled_cpu_ns": 100,
            "lost_records": 0,
            "unresolved_leaf_samples": 0,
            "unrooted_samples": 0,
            "stages_cpu_ns": {"auth": 100},
            "hotspots": [],
        }

        def profiled(config: RunConfig, output: Path) -> int:
            code = write_cli_workload(config, output)
            write_json(output / "perf-analysis.json", profile)
            write_json(output / "window.json", {"hold_s": 60})
            return code

        with TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(
                invoke_cli(
                    self, root, ["bench", "--perf", "user", "--label", "perf"], workload=profiled
                ),
                0,
            )
            path = canonical_path(root, "perf")
            self.assertEqual(read_json(path / "raw" / "perf-analysis.json"), profile)
            saved = mapping(read_json(path / "summary.json"))
            self.assertEqual(saved["profile"], profile)
            self.assertEqual(mapping(saved["benchmark"])["name"], "perf")

    def test_unknown_result_format_fails_explicitly(self) -> None:
        with TemporaryDirectory() as directory:
            path = Path(directory) / "unsupported.json"
            write_json(path, {"unrecognized": True})
            with (
                patch("sys.argv", ["dev.py", "results", str(path)]),
                patch("sys.stderr", StringIO()) as stderr,
            ):
                with self.assertRaises(SystemExit) as stopped:
                    dev.main()
            self.assertEqual(stopped.exception.code, 1)
            self.assertIn("unsupported", stderr.getvalue().lower())

    def test_default_results_selects_the_current_anchor(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory)
            anchor = root / "src" / "runs" / CLI_ANCHOR[:7]
            anchor.mkdir(parents=True)
            with (
                patch("sys.argv", ["dev.py", "results", "--json"]),
                patch("devtools.runtime.ROOT", root),
                patch("devtools.runtime.command", return_value=CLI_ANCHOR),
                patch("devtools.reports.load_saved_report", return_value=[]) as read,
                patch("builtins.print"),
            ):
                dev.main()
                read.assert_called_once_with(anchor)

    def test_default_results_is_empty_before_this_anchor_has_runs(self) -> None:
        with TemporaryDirectory() as directory:
            with (
                patch("sys.argv", ["dev.py", "results", "--json"]),
                patch("devtools.runtime.ROOT", Path(directory)),
                patch("devtools.runtime.command", return_value=CLI_ANCHOR),
                patch("devtools.reports.load_saved_report") as read,
                patch("builtins.print") as output,
            ):
                dev.main()
                read.assert_not_called()
                output.assert_called_once_with("[]")


class ToolBoundaryTests(unittest.TestCase):
    def test_workload_settings_are_explicit_and_duration_parsing_is_cached(self) -> None:
        config = RunConfig(
            label="cached", vus=7, ramp="15s", hold="45s", seed=42, telemetry="profile", perf="all"
        )
        self.assertEqual(
            config.workload,
            {"generator": "cpp", "vus": 7, "ramp": "15s", "hold": "45s", "seed": 42},
        )
        with patch("devtools.reports.duration_seconds", wraps=duration_seconds) as parse:
            self.assertEqual(config.timing, (15, 45))
            self.assertEqual(config.timing, (15, 45))
            self.assertEqual([call.args for call in parse.call_args_list], [("15s",), ("45s",)])

    def test_summary_uses_workload_and_only_current_cgroup_client_memory(self) -> None:
        config = RunConfig(label="summary", vus=7, ramp="15s", hold="45s", seed=42)
        with TemporaryDirectory() as directory:
            output = Path(directory)
            write_cli_workload(config, output)
            run = mapping(read_json(output / "run.json"))
            run["arguments"] = ["VUS=999", "DURATION=1s", "RAMP_UP=2s"]
            run["load_generator"] = "native"
            write_json(output / "run.json", run)
            write_json(
                output / "samples.json",
                [
                    {"client_rss_bytes": 999999, "client_cgroup_bytes": 4096},
                    {"client_rss_bytes": 9999999},
                    {"client_cgroup_bytes": 8192},
                ],
            )
            result = reports.summarize(output).result
            self.assertEqual((result["users"], result["hold"], result["ramp"]), (7, "45s", "15s"))
            self.assertEqual(result.get("client_sampled_peak_bytes"), 8192)
            del run["arguments"]
            write_json(output / "run.json", run)
            write_json(output / "samples.json", [{"client_rss_bytes": 9999999}])
            result = reports.summarize(output).result
            self.assertEqual(result["users"], 7)
            self.assertNotIn("client_sampled_peak_bytes", result)

    def test_client_diagnostics_and_seed_are_rejected_for_k6_before_resources(self) -> None:
        for option in (["--client-diagnostics"], ["--seed", "7"]):
            with (
                self.subTest(option=option),
                patch(
                    "sys.argv",
                    ["dev.py", "bench", "--label", "invalid", "--generator", "k6", *option],
                ),
                patch("sys.stderr", StringIO()),
                patch("devtools.runtime.command") as command,
                patch("devtools.sources.capture") as capture,
            ):
                with self.assertRaises(SystemExit) as stopped:
                    dev.main()
                self.assertEqual(stopped.exception.code, 2)
                command.assert_not_called()
                capture.assert_not_called()

    def test_obsolete_commands_and_flags_are_rejected(self) -> None:
        options = (
            ["profile", "--label", "old"],
            ["bench", "--label", "old", "--scope", "user"],
            ["bench", "--label", "old", "--k6", "k6.exe"],
            ["bench", "--label", "old", "--no-build"],
        )
        for arguments in options:
            with self.subTest(arguments=arguments), patch("sys.stderr", StringIO()):
                with self.assertRaises(SystemExit) as stopped:
                    dev.build_parser().parse_args(arguments, namespace=dev.Arguments())
                self.assertEqual(stopped.exception.code, 2)

    def test_parser_creates_an_immutable_workload_snapshot_without_cli_options(self) -> None:
        modes: tuple[runtime.PerfMode, ...] = ("off", "user", "kernel", "all")
        for perf in modes:
            expected_vus = 350000 if perf == "off" else 5000
            with self.subTest(perf=perf):
                args = dev.build_parser().parse_args(
                    ["bench", "--label", "snapshot", "--perf", perf], namespace=dev.Arguments()
                )
                config = dev.workload_config(args)
                self.assertIsInstance(config, RunConfig)
                self.assertEqual(config.perf, perf)
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
                    any(
                        hasattr(config, name)
                        for name in ("mode", "scope", "k6", "save", "no_build", "image", "docker")
                    )
                )

    def test_invalid_cli_workload_preserves_exit_codes_without_starting_resources(self) -> None:
        for options, exit_code in ((["--vus", "0"], 2), (["--hold", "0s"], 1)):
            with (
                self.subTest(options=options),
                patch("sys.argv", ["dev.py", "bench", "--label", "invalid", *options]),
                patch("sys.stderr", StringIO()),
                patch("devtools.runtime.run_workload") as run,
                patch("devtools.runs.create_run") as create,
            ):
                with self.assertRaises(SystemExit) as stopped:
                    dev.main()
                self.assertEqual(stopped.exception.code, exit_code)
                run.assert_not_called()
                create.assert_not_called()

    def test_removed_export_flag_cannot_overwrite_historical_measurements(self) -> None:
        with TemporaryDirectory() as directory:
            destination = Path(directory) / "historical.json"
            original = b'[{"old_measurement":true}]\n'
            destination.write_bytes(original)
            with (
                patch(
                    "sys.argv", ["dev.py", "bench", "--label", "export", "--save", str(destination)]
                ),
                patch("sys.stderr", StringIO()),
                patch("devtools.runtime.command") as command,
            ):
                with self.assertRaises(SystemExit) as stopped:
                    dev.main()
                self.assertEqual(stopped.exception.code, 2)
                command.assert_not_called()
            self.assertEqual(destination.read_bytes(), original)

    def test_cli_reuses_the_report_summary_for_diagnostics(self) -> None:
        metadata: dict[str, JSONValue] = {"seed": 42, "workflow_errors": 0}

        with TemporaryDirectory() as directory:
            root = Path(directory)
            with (
                patch("devtools.reports.read_json", wraps=reports.read_json) as read,
            ):
                code = invoke_cli(
                    self,
                    root,
                    [
                        "bench",
                        "--label",
                        "single-read",
                        "--vus",
                        "7",
                        "--telemetry",
                        "profile",
                        "--client-diagnostics",
                        "--image",
                        "old-runtime",
                    ],
                )
                self.assertEqual(code, 0)
                raw = canonical_path(root, "single-read") / "raw" / "k6-summary.json"
                self.assertEqual(sum(call.args == (raw,) for call in read.call_args_list), 1)
            saved = mapping(read_json(canonical_path(root, "single-read") / "summary.json"))
            result = mapping(saved["benchmark"])
            self.assertEqual(result["name"], "single-read")
            self.assertEqual(result["cpu_scope"], "server_cgroup")
            self.assertEqual(result["cpu_us_per_request"], 10)
            self.assertEqual(saved["load_generator"], metadata)
            self.assertEqual(
                (canonical_path(root, "single-read") / "source.patch").read_bytes(), CLI_PATCH
            )

    def test_workload_failure_unwinds_generator_observer_and_server(self) -> None:
        cleanup: list[str] = []
        with (
            patch(
                "devtools.images.ensure_generator", return_value=make_identity("generator-pinned")
            ),
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
                runtime.run_workload(
                    config,
                    Path("unused"),
                    image=make_identity(),
                    source=make_snapshot(runtime.ROOT),
                )
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
    def test_integer_and_histogram_summary_preserve_large_json_integers(self) -> None:
        for value in (2**53 + 1, 2**64 - 1):
            with self.subTest(value=value):
                self.assertEqual(reports.integer({"count": value}, "count"), value)
                histogram: dict[str, JSONValue] = {
                    "unit": "ns",
                    "count": value,
                    "total": value,
                    "max": value,
                    "buckets": [],
                }
                raw: dict[str, JSONValue] = {"duration": histogram}
                original = json.dumps(raw)
                compact = mapping(reports.metric_summary(raw)["duration"])
                self.assertEqual(compact["count"], value)
                self.assertEqual(compact["max"], value)
                self.assertEqual(compact["mean"], 1)
                self.assertEqual(json.dumps(raw), original)

    def test_integer_keeps_integral_float_support_and_rejects_nonintegers(self) -> None:
        self.assertEqual(reports.integer({"count": 3.0}, "count"), 3)
        self.assertEqual(reports.number({"duration": 3.25}, "duration"), 3.25)
        for value in (True, False, 3.25, float("inf"), float("nan"), "3"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                reports.integer({"count": value}, "count")

    def test_probe_kind_takes_precedence_over_legacy_runs_field(self) -> None:
        document: dict[str, JSONValue] = {
            "kind": "telemetry_hook_probe",
            "runs": [{"variant": "before"}],
            "medians": {"before": {"io_ns_per_call": 12}, "after": {"io_ns_per_call": 9}},
            "reduction_percent": {"io_ns_per_call": 25},
        }
        with patch("sys.stdout", new_callable=StringIO) as output:
            reports.show_saved_report(document)
        self.assertIn("12.00", output.getvalue())
        self.assertIn("9.00", output.getvalue())
        self.assertIn("25.00%", output.getvalue())

    def test_workload_views_show_missing_owners_and_retained_timings(self) -> None:
        telemetry = parse_telemetry(telemetry_record("database"))
        telemetry["reports"][0]["totals"] = {
            "metrics": {
                "write_commit_ns": {
                    "unit": "ns",
                    "count": 2,
                    "total": 5000,
                    "max": 4000,
                    "buckets": [],
                }
            }
        }
        compact = reports.telemetry_summary(decode_json(json.dumps(telemetry).encode()))
        documents: tuple[JSONValue, ...] = (
            {"load_generator": {}, "server_telemetry": compact},
            {
                "kind": "workload",
                "schema_version": 1,
                "label": "missing-owners",
                "status": "passed",
                "load_generator": {},
                "server_telemetry": compact,
            },
        )
        for document in documents:
            with (
                self.subTest(document=document),
                patch("sys.stdout", new_callable=StringIO) as output,
            ):
                reports.show_saved_report(document)
            rendered = output.getvalue()
            self.assertIn("incomplete", rendered)
            self.assertIn("transport", rendered)
            self.assertIn("checkpoint", rendered)
            self.assertIn("write_commit", rendered)
            self.assertIn("mean=2.500", rendered)
            self.assertIn("max=4.000 us", rendered)

    def test_summary_compacts_nested_sync_and_ring_histograms_without_losing_counters(self) -> None:
        histogram: dict[str, JSONValue] = {
            "unit": "ns",
            "count": 2,
            "total": 17,
            "max": 12,
            "buckets": [[3, 1], [4, 1]],
        }
        document = parse_telemetry(telemetry_record("transport", overwritten_windows=1))
        document["reports"][0]["totals"] = {
            "metrics": {"sync": {"wall_ns": histogram}},
            "rings": [{"id": 3, "enobufs": 7, "metrics": {"send_ns": histogram}}],
        }
        original = json.dumps(document)
        compact = reports.telemetry_summary(decode_json(json.dumps(document).encode()))
        owners = compact["owners"]
        assert isinstance(owners, list)
        owner = mapping(owners[0])
        sync = mapping(mapping(mapping(owner["metrics"])["sync"])["wall_ns"])
        self.assertEqual((sync["mean"], sync["max"], sync["count"]), (8.5, 12, 2))
        rings = owner["rings"]
        assert isinstance(rings, list)
        ring = mapping(rings[0])
        self.assertEqual((ring["id"], ring["enobufs"]), (3, 7))
        self.assertEqual(mapping(mapping(ring["metrics"])["send_ns"]), sync)
        self.assertFalse(compact["complete"])
        self.assertEqual(compact["overwritten_windows"], 1)
        self.assertNotIn('"buckets"', json.dumps(compact))
        self.assertEqual(json.dumps(document), original)

    def test_summary_keeps_other_results_when_optional_artifacts_are_malformed(self) -> None:
        config = RunConfig(label="partial", vus=5, ramp="1s", hold="5s", telemetry="profile")
        with TemporaryDirectory() as directory:
            output = Path(directory)
            write_cli_workload(config, output)
            (output / "perf-analysis.json").write_text("{broken", encoding="utf-8")
            (output / "telemetry.jsonl").write_text('{"source":', encoding="utf-8")
            write_json(output / "window.json", {"start": 1, "end": 5})
            write_json(output / "collection-errors.json", ["cleanup failed"])
            report = reports.run_summary(output, label="partial", telemetry="profile")
        self.assertTrue(mapping(report["benchmark"])["passed"])
        self.assertEqual(report["window"], {"start": 1, "end": 5})
        errors = report["collection_errors"]
        assert isinstance(errors, list)
        self.assertEqual(len(errors), 3)
        self.assertIn("cleanup failed", errors)
        self.assertTrue(any("perf-analysis.json" in str(error) for error in errors))
        self.assertTrue(any("telemetry" in str(error).lower() for error in errors))

    def test_compact_telemetry_and_pretty_summary_preserve_json_values(self) -> None:
        value = {"counter": 2**64 - 1, "label": 'quote" and newline\n'}
        with TemporaryDirectory() as directory:
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
            with (
                self.subTest(mode=mode),
                patch("devtools.images._command") as command,
                patch("devtools.images.inspect", return_value=make_identity()) as inspect,
                patch(
                    "devtools.runtime.subprocess.run",
                    return_value=subprocess.CompletedProcess([], 0, "", ""),
                ),
            ):
                source = make_snapshot(runtime.ROOT)
                image = images.build(source, telemetry=mode)
                self.assertIs(image, inspect.return_value)
                self.assertIn(
                    f"twelve-dollar-cpp:{CLI_ANCHOR[:12]}-{source.patch_sha256[:12]}-{'c' * 12}",
                    command.call_args_list[1].args,
                )
                invocation = command.call_args_list[0].args
                self.assertEqual(invocation[:2], ("docker", "build"))
                self.assertIn(f"--build-arg=TDS_TELEMETRY={mode}", invocation)
                self.assertNotIn("--build-arg=TDS_DIAGNOSTICS=ON", invocation)
        with (
            patch("devtools.runtime.server") as server_context,
            patch("devtools.runtime.observing") as observer,
            patch("devtools.runtime.load_generator"),
            patch("devtools.runtime.load", return_value=0),
            patch("devtools.images.inspect", return_value=make_identity("k6-pinned")),
        ):
            server_context.return_value.__enter__.return_value = ("server", 3000)
            config = RunConfig(
                label="profile-off", vus=1, ramp="30s", hold="60s", perf="user", generator="k6"
            )
            image = make_identity()
            profiler = make_identity("profiler-pinned")
            runtime.run_workload(
                config,
                Path("unused"),
                image=image,
                source=make_snapshot(runtime.ROOT),
                profiler_image=profiler,
            )
            self.assertTrue(observer.call_args.kwargs["profiling"])
            self.assertIs(observer.call_args.kwargs["profiler_image"], profiler)
            server_context.assert_called_once_with(
                Path("unused"), image=image, telemetry="off", workspace=runtime.ROOT
            )

    def test_export_is_copied_after_stop_and_before_container_removal(self) -> None:
        events: list[str] = []

        def command(*args: str, timeout: int | None = 30) -> str:
            del timeout
            if args[:2] == ("docker", "port"):
                return "127.0.0.1:12345"
            if args[-1] == "--version":
                return '{"telemetry":"profile"}'
            if args[:2] == ("docker", "inspect"):
                return '[{"Image":"server-pinned"}]'
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
            TemporaryDirectory() as directory,
            patch("devtools.runtime.command", side_effect=command),
            patch("devtools.runtime.healthy", return_value=True),
            patch("devtools.runtime.subprocess.run", side_effect=run),
        ):
            output = Path(directory)
            seed = output / "fixture.db"
            seed.touch()
            with server(output, image=make_identity(), database=seed, telemetry="profile"):
                pass
            self.assertLess(events.index("stop"), events.index("cp"))
            self.assertLess(events.index("cp"), events.index("rm"))
            saved = parse_telemetry(
                (output / "telemetry.jsonl").read_text(encoding="utf-8"), expected_mode="profile"
            )
            self.assertTrue(saved["complete"])
            self.assertFalse((output / "telemetry.json").exists())
            self.assertTrue((output / "telemetry.jsonl").is_file())

    def test_missing_export_does_not_mask_workload_failure_or_cleanup(self) -> None:
        def command(*args: str, timeout: int | None = 30) -> str:
            del timeout
            if args[:2] == ("docker", "port"):
                return "127.0.0.1:12345"
            if args[:2] == ("docker", "inspect"):
                return '[{"Image":"server-pinned"}]'
            return '{"telemetry":"basic"}' if args[-1] == "--version" else "{}"

        def run(args: list[str], **_options: object) -> subprocess.CompletedProcess[str]:
            return subprocess.CompletedProcess(
                args, 1 if args[1] == "cp" else 0, "[]", "export missing" if args[1] == "cp" else ""
            )

        with (
            TemporaryDirectory() as directory,
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
                with server(output, image=make_identity(), database=seed, telemetry="basic") as (
                    name,
                    _port,
                ):
                    container = name
                    raise RuntimeError("workload failed")
            errors = read_json(output / "collection-errors.json")
            self.assertIn("export missing", str(errors))
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
            TemporaryDirectory() as directory,
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
                with server(output, image=make_identity(), database=seed, telemetry="off"):
                    entered = True
            self.assertFalse(entered)

    def test_truncated_artifact_stays_raw_and_collection_is_unsuccessful(self) -> None:
        def copied(args: list[str], **_options: object) -> subprocess.CompletedProcess[str]:
            Path(args[-1]).write_text(
                telemetry_record("transport") + '{"source":', encoding="utf-8"
            )
            return subprocess.CompletedProcess(args, 0, "", "")

        with (
            TemporaryDirectory() as directory,
            patch("devtools.runtime.subprocess.run", side_effect=copied),
        ):
            output = Path(directory)
            runtime.collect_telemetry("stopped-server", output)
            report = reports.run_summary(output, label="truncated", telemetry="profile")
            self.assertTrue(report["collection_errors"])
            self.assertTrue((output / "telemetry.jsonl").read_text().endswith('{"source":'))
            self.assertFalse((output / "telemetry.json").exists())
