"""Check weighted native profiles, stack attribution and incomplete captures."""

import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from dev import (
    Arguments,
    JSONValue,
    LoadGenerator,
    Result,
    analyze_profile,
    append_result,
    cpu_delta,
    decode_json,
    duration_seconds,
    kernel_stage,
    load,
    load_generator,
    mapping,
    parse_counters,
    parse_timings,
    profile_stage,
    read_json,
    server,
    write_json,
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
                patch("dev.command", side_effect=["127.0.0.1:6565", "cpp-version\nsha /app/load"]),
                patch(
                    "dev.subprocess.run", return_value=subprocess.CompletedProcess([], 0, "[]", "")
                ),
            ):
                output = Path(directory)
                write_json(output / "environment.json", {"docker_info": {"MemTotal": 24 * 2**30}})
                args = Arguments(generator="cpp", k6=None, vus=1, hold="60s", ramp="30s", seed=seed)
                with load_generator(args, "test-container", 3000, output) as generator:
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
        args = Arguments(action="bench", label="exit-race", ramp="30s", hold="60s")
        generator = LoadGenerator(["k6"], 6565, "test", "test", "load-container")
        snapshot: dict[str, JSONValue] = {
            "user_usec": 0,
            "system_usec": 0,
            "main_user_ticks": 0,
            "main_system_ticks": 0,
        }
        with (
            tempfile.TemporaryDirectory() as directory,
            patch("dev.subprocess.Popen") as launch,
            patch("dev.command", return_value="100"),
            patch("dev.k6_status", return_value={"status": 4}),
            patch("dev.counters", return_value=snapshot),
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
        args = Arguments(action="bench", label="native-gate", ramp="30s", hold="60s")
        generator = LoadGenerator(["load_generator"], 0, "test", "test", kind="cpp")
        snapshot: dict[str, JSONValue] = {
            "user_usec": 0,
            "system_usec": 0,
            "main_user_ticks": 0,
            "main_system_ticks": 0,
        }
        with (
            tempfile.TemporaryDirectory() as directory,
            patch("dev.subprocess.Popen") as launch,
            patch("dev.command", return_value="100"),
            patch("dev.k6_status") as k6_api,
            patch("dev.counters", return_value=snapshot),
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

    def test_timing_rows_remain_separate_from_server_messages(self) -> None:
        log = (
            "Listening on 0.0.0.0:3000\n"
            'TDS_TIMING {"source":"transport","time_ns":123,"final":false}\n'
            "WAL checkpoint busy, retrying on a later write\n"
            'TDS_TIMING {"owner":"checkpoint","time_ns":125,"final":true}\n'
        )
        rows = parse_timings(log)
        self.assertEqual([row["time_ns"] for row in rows], [123, 125])
        self.assertEqual(rows[-1]["final"], True)
        self.assertEqual(parse_timings("Listening\n"), [])
        with self.assertRaises(ValueError):
            parse_timings("TDS_TIMING not-json")

    def test_stop_timeout_preserves_failure_and_removes_owned_resources(self) -> None:
        def response(*args: str) -> str:
            return "127.0.0.1:12345" if args[:2] == ("docker", "port") else "{}"

        with (
            tempfile.TemporaryDirectory() as directory,
            patch("dev.command", side_effect=response) as command,
            patch("dev.healthy", return_value=True),
            patch("dev.subprocess.run") as run,
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
