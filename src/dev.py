"""Checks, Docker benchmarks and saved results. Run with uv."""

import argparse
import hashlib
import http.client
import json
import math
import os
import platform
import re
import shlex
import shutil
import socket
import subprocess
import tarfile
import tempfile
import time
import uuid
from collections import Counter
from collections.abc import Generator
from contextlib import closing, contextmanager
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Literal, Never, TypedDict, cast

import psutil

ROOT = Path(__file__).resolve().parent.parent
IMAGE = "twelve-dollar-cpp:local"
PROFILER_IMAGE = "twelve-dollar-profiler:local"
K6_IMAGE = (
    "grafana/k6:1.3.0@sha256:a90b459a3768c46ad1013da53af24189f735d7112273c6ac3212ca8ed0e18656"
)
PROFILE_EVENT = "cpu-clock:u"
PROFILE_FREQUENCY = 997
PROFILE_STACK_BYTES = 8192
K6_PAUSED_BEFORE_RUN = 4
PROFILE_STAGES = {
    "database_and_rows": "SQL / row assembly",
    "transport_and_other": "Transport / other",
    "response_json": "Response JSON",
    "request_json": "Request JSON",
    "auth": "Auth / JWT JSON",
    "unknown": "Unknown",
}
JWT_KEY = "twelve-dollar-challenge"
RESULTS = ROOT / "src/benchmarks/results.json"


type JSONValue = bool | int | float | str | list[JSONValue] | dict[str, JSONValue] | None


def reject_constant(value: str) -> Never:
    raise ValueError(f"invalid JSON constant: {value}")


def decode_json(data: bytes) -> JSONValue:
    return cast(JSONValue, json.loads(data.decode("utf-8"), parse_constant=reject_constant))


class Arguments(argparse.Namespace):
    action: str
    docker: bool
    label: str
    vus: int
    hold: str
    ramp: str
    k6: Path | None
    save: Path | None
    file: Path
    json_output: bool
    no_build: bool


class Result(TypedDict):
    date: str
    name: str
    runtime: str
    load_generator: str
    sqlite: str
    users: int
    hold: str
    ramp: str
    requests: int
    rps: float
    p95_ms: float
    p99_ms: float
    max_ms: float
    error_rate: float
    cpu_us_per_request: float
    cpu_scope: str
    server_peak_bytes: int
    passed: bool


class Hotspot(TypedDict):
    symbol: str
    module: str
    cpu_ns: int
    samples: int


class NativeProfile(TypedDict):
    event: str
    frequency_hz: int
    stack_bytes: int
    samples: int
    sampled_cpu_ns: int
    lost_records: int
    unresolved_leaf_samples: int
    unrooted_samples: int
    stages_cpu_ns: dict[str, int]
    hotspots: list[Hotspot]


def command(*args: str, timeout: int | None = 30) -> str:
    result = subprocess.run(
        args, check=True, capture_output=True, text=True, encoding="utf-8", timeout=timeout
    )
    return result.stdout.strip()


def write_json(path: Path, value: object) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def file_hash(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def parse_counters(values: str) -> dict[str, JSONValue]:
    rows, process = values.rsplit("process_stat ", 1)
    fields = process[process.rindex(")") + 2 :].split()
    result: dict[str, JSONValue] = {
        line.split()[0]: int(line.split()[1]) for line in rows.splitlines()
    }
    result["main_user_ticks"] = int(fields[11])
    result["main_system_ticks"] = int(fields[12])
    return result


def counters(observer: str) -> dict[str, JSONValue]:
    # Read through the server's root, but run the reader in a separate cgroup.
    root = "/proc/1/root/sys/fs/cgroup"
    values = command(
        "docker",
        "exec",
        observer,
        "sh",
        "-c",
        f"cat {root}/cpu.stat; "
        f"printf 'memory_current '; cat {root}/memory.current; "
        f"printf 'memory_peak '; cat {root}/memory.peak; "
        f"cat {root}/memory.events; printf 'process_stat '; cat /proc/1/stat",
    )
    return parse_counters(values)


def cpu_delta(
    before: dict[str, JSONValue], after: dict[str, JSONValue], ticks_per_second: int
) -> dict[str, float]:
    def delta(key: str) -> float:
        return number(after, key) - number(before, key)

    user = delta("user_usec") / 1e6
    system = delta("system_usec") / 1e6
    main_user = delta("main_user_ticks") / ticks_per_second
    main_system = delta("main_system_ticks") / ticks_per_second
    return {
        "cgroup_user_s": user,
        "cgroup_system_s": system,
        "main_user_s": main_user,
        "main_system_s": main_system,
        "cgroup_minus_main_s": user + system - main_user - main_system,
    }


def duration_seconds(value: str) -> float:
    units = {"ns": 1e-9, "us": 1e-6, "ms": 1e-3, "s": 1, "m": 60, "h": 3600}
    parts = list(re.finditer(r"(\d+(?:\.\d+)?)(ns|us|ms|s|m|h)", value))
    if "".join(part[0] for part in parts) != value or not parts:
        raise ValueError(f"invalid duration: {value}")
    seconds = sum(float(part[1]) * units[part[2]] for part in parts)
    if seconds <= 0 or not math.isfinite(seconds):
        raise ValueError("durations must be finite and positive")
    return seconds


def k6_status(port: int, *, resume: bool = False) -> dict[str, JSONValue]:
    body = (
        b'{"data":{"type":"status","id":"default","attributes":{"paused":false}}}'
        if resume
        else None
    )
    with closing(http.client.HTTPConnection("127.0.0.1", port, timeout=2)) as connection:
        connection.request(
            "PATCH" if resume else "GET",
            "/v1/status",
            body=body,
            headers={"Content-Type": "application/json"},
        )
        response = connection.getresponse()
        data = response.read()
        if response.status != 200:
            raise RuntimeError(f"k6 control API returned {response.status}: {data!r}")
        return mapping(mapping(mapping(decode_json(data))["data"])["attributes"])


def perf_control(observer: str, action: Literal["ping", "enable", "disable"]) -> None:
    command(
        "docker",
        "exec",
        observer,
        "bash",
        "-c",
        'exec 3<>/tmp/perf-ack 4<>/tmp/perf-control; printf "%s\\n" "$1" >&4; '
        'read -r -t 5 reply <&3 && test "$reply" = ack',
        "perf-control",
        action,
    )


def healthy(port: int, *, expected: int = 200) -> bool:
    try:
        with closing(http.client.HTTPConnection("127.0.0.1", port, timeout=2)) as connection:
            connection.request("GET", "/health")
            response = connection.getresponse()
            response.read()
            return response.status == expected
    except OSError, http.client.HTTPException:
        return False


def read_json(path: Path) -> JSONValue:
    return decode_json(path.read_bytes())


def mapping(value: JSONValue) -> dict[str, JSONValue]:
    if not isinstance(value, dict):
        raise ValueError("expected a JSON object")
    return value


def number(row: dict[str, JSONValue], key: str) -> float:
    value = row[key]
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f"{key}: expected a finite number")
    return float(value)


def integer(row: dict[str, JSONValue], key: str) -> int:
    value = number(row, key)
    if not value.is_integer():
        raise ValueError(f"{key}: expected an integer")
    return int(value)


def text(row: dict[str, JSONValue], key: str) -> str:
    value = row[key]
    if not isinstance(value, str):
        raise ValueError(f"{key}: expected text")
    return value


def read_results(path: Path) -> list[Result]:
    rows = read_json(path)
    if not isinstance(rows, list):
        raise ValueError("results must be a JSON array")
    results: list[Result] = []
    for value in rows:
        row = mapping(value)
        passed = row["passed"]
        if not isinstance(passed, bool):
            raise ValueError("passed: expected a boolean")
        results.append(
            Result(
                date=text(row, "date"),
                name=text(row, "name"),
                runtime=text(row, "runtime"),
                load_generator=text(row, "load_generator") if "load_generator" in row else "native",
                sqlite=text(row, "sqlite"),
                users=integer(row, "users"),
                hold=text(row, "hold"),
                ramp=text(row, "ramp"),
                requests=integer(row, "requests"),
                rps=number(row, "rps"),
                p95_ms=number(row, "p95_ms"),
                p99_ms=number(row, "p99_ms"),
                max_ms=number(row, "max_ms"),
                error_rate=number(row, "error_rate"),
                cpu_us_per_request=number(row, "cpu_us_per_request"),
                cpu_scope=text(row, "cpu_scope") if "cpu_scope" in row else "server_and_sampler",
                server_peak_bytes=integer(row, "server_peak_bytes"),
                passed=passed,
            )
        )
    return results


def summarize(output: Path) -> Result:
    run = mapping(read_json(output / "run.json"))
    summary = mapping(read_json(output / "k6-summary.json"))
    environment = mapping(read_json(output / "environment.json"))
    runtime = mapping(environment["runtime"])
    metrics = mapping(summary["metrics"])
    requests = mapping(metrics["http_reqs"])
    latency = mapping(metrics["http_req_duration"])
    errors = mapping(metrics["http_req_failed"])
    before = mapping(run["cgroup_before"])
    after = mapping(run["cgroup_after"])
    arguments = run["arguments"]
    if not isinstance(arguments, list):
        raise ValueError("run arguments must be an array")

    def setting(key: str) -> str:
        prefix = key + "="
        return next(
            arg[len(prefix) :]
            for arg in arguments
            if isinstance(arg, str) and arg.startswith(prefix)
        )

    count = integer(requests, "count")
    if count <= 0:
        raise ValueError("no HTTP requests were measured")
    return Result(
        date=datetime.fromtimestamp((output / "run.json").stat().st_mtime, UTC).date().isoformat(),
        name=output.name,
        runtime=f"C++ {text(runtime, 'version')}",
        load_generator=text(run, "load_generator") if "load_generator" in run else "native",
        sqlite=text(runtime, "sqlite"),
        users=int(setting("VUS")),
        hold=setting("DURATION"),
        ramp=setting("RAMP_UP"),
        requests=count,
        rps=number(requests, "rate"),
        p95_ms=number(latency, "p(95)"),
        p99_ms=number(latency, "p(99)"),
        max_ms=number(latency, "max"),
        error_rate=number(errors, "value"),
        cpu_us_per_request=(number(after, "usage_usec") - number(before, "usage_usec")) / count,
        cpu_scope=text(run, "cpu_scope") if "cpu_scope" in run else "server_and_sampler",
        server_peak_bytes=integer(after, "memory_peak"),
        passed=(
            integer(run, "exit_code") == 0
            and number(mapping(metrics["checks"]), "fails") == 0
            and integer(after, "oom_kill") == 0
        ),
    )


def show_results(results: list[Result]) -> None:
    rows = [
        [
            "Run",
            "Runtime",
            "Client",
            "Users",
            "Hold",
            "Req/s",
            "p95 ms",
            "p99 ms",
            "Errors",
            "CPU us/req",
            "CPU scope",
            "RAM MiB",
            "Pass",
        ]
    ]
    for row in results:
        rows.append(
            [
                f"{row['date']}/{row['name']}",
                row["runtime"],
                "loopback" if row["load_generator"] == "docker_loopback" else "native",
                str(row["users"]),
                row["hold"],
                f"{row['rps']:.2f}",
                f"{row['p95_ms']:.3f}",
                f"{row['p99_ms']:.3f}",
                f"{row['error_rate']:.2%}",
                f"{row['cpu_us_per_request']:.2f}",
                "server" if row["cpu_scope"] == "server_cgroup" else "server+sampler",
                f"{row['server_peak_bytes'] / 2**20:.2f}",
                "yes" if row["passed"] else "NO",
            ]
        )
    widths = [max(len(row[i]) for row in rows) for i in range(len(rows[0]))]
    for row in rows:
        print(
            "  ".join(
                value.ljust(widths[i]) if i == 0 else value.rjust(widths[i])
                for i, value in enumerate(row)
            )
        )


def build(*, tests: bool = False, profiler: bool = False) -> str:
    print("Building profiler image..." if profiler else "Building C++ image...", flush=True)
    image = PROFILER_IMAGE if profiler else IMAGE + ("-build" if tests else "")
    command(
        "docker",
        "build",
        "--progress",
        "plain",
        "--platform",
        "linux/amd64",
        "--tag",
        image,
        *(["--target", "profiler"] if profiler else ["--target", "build"] if tests else []),
        "--file",
        str(ROOT / "src/Dockerfile"),
        str(ROOT / "src"),
        timeout=None,
    )
    return image


def cpp_editor(image: str) -> None:
    """Give Windows IntelliSense the headers used by the Linux build."""
    destination = ROOT / "src/.build/editor"
    destination.mkdir(parents=True, exist_ok=True)

    version = command("docker", "run", "--rm", image, "g++", "-dumpversion")
    target = command("docker", "run", "--rm", image, "g++", "-dumpmachine")
    gcc = command("docker", "run", "--rm", image, "g++", "-print-file-name=include")
    directories = {
        "cxx": f"/usr/include/c++/{version}",
        "cxx-target": f"/usr/include/{target}/c++/{version}",
        "gcc": gcc,
        "local": "/usr/local/include",
        "system": "/usr/include",
        "uring": "/build/out/_deps/liburing/src/include",
        "llhttp": "/build/out/_deps/llhttp-src/include",
    }
    script = "set -eu\nmkdir -p /tmp/editor/sqlite /tmp/editor/yyjson\n"
    for name, source in directories.items():
        script += f"cp -aL {shlex.quote(source)} /tmp/editor/{name}\n"
    script += (
        "cp /build/out/_deps/sqlite-src/sqlite3.h /build/out/_deps/sqlite-src/sqlite3ext.h "
        "/tmp/editor/sqlite/\n"
        "cp /build/out/_deps/yyjson_source-src/src/yyjson.h /tmp/editor/yyjson/\n"
        "tar -cf - -C /tmp/editor .\n"
    )
    with tempfile.TemporaryFile() as archive:
        subprocess.run(
            ["docker", "run", "--rm", "--network", "none", image, "sh", "-c", script],
            stdout=archive,
            check=True,
        )
        archive.seek(0)
        with tarfile.open(fileobj=archive) as headers:
            headers.extractall(destination, filter="data")

    print("Editor headers refreshed from the Linux build image.")


def native_check() -> None:
    image = build(tests=True)
    cpp_editor(image)
    for executable, arguments in (("server", ["--self-test"]), ("transport_tests", [])):
        print(
            command(
                "docker",
                "run",
                "--rm",
                "--network",
                "none",
                "--read-only",
                "--user",
                "10001:10001",
                "--cap-drop",
                "ALL",
                "--cpus",
                "1",
                "--memory",
                "2g",
                "--memory-swap",
                "2g",
                "--ulimit",
                "nofile=1048576:1048576",
                "--security-opt",
                f"seccomp={ROOT / 'src/docker/seccomp.json'}",
                "--entrypoint",
                f"/build/out/{executable}",
                image,
                *arguments,
            )
        )


def check(use_docker: bool) -> None:
    for args in [
        ["ruff", "check", "."],
        ["ruff", "format", "--check", "."],
        ["pyright"],
        ["python", "-m", "unittest", "-v", "checks.test_profile"],
    ]:
        subprocess.run(["uv", "run", "--locked", *args], cwd=ROOT / "src", check=True)
    if use_docker:
        build()
        native_check()
        output = ROOT / "results"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="check-", dir=output) as directory:
            work = Path(directory).resolve()
            with server(work) as (container, port):
                verify(container, port, work)
        subprocess.run(
            ["uv", "run", "--locked", "python", "-m", "unittest", "-v", "checks.test_api"],
            cwd=ROOT / "src",
            env=os.environ | {"TDS_HTTP_TESTS": "1"},
            check=True,
        )


def verify(container: str, port: int, output: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="official-", dir=output) as directory:
        work = Path(directory).resolve()
        if not work.is_relative_to(output.resolve()):
            raise RuntimeError("official check directory must stay under its output directory")
        # Only normalize checkout CRLF. Preserve every assertion and golden byte.
        (work / "test.sh").write_text(
            (ROOT / "test/test.sh").read_text(encoding="utf-8"),
            encoding="utf-8",
            newline="\n",
        )
        shutil.copytree(ROOT / "test/golden", work / "golden")
        command("docker", "cp", str(work), f"{container}:/tmp/official")
        result = command("docker", "exec", container, "bash", "/tmp/official/test.sh")
        (output / "official-tests.txt").write_text(result + "\n", encoding="utf-8")
        if "passed=42 failed=0" not in result:
            raise RuntimeError(result)
    with closing(http.client.HTTPConnection("127.0.0.1", port, timeout=5)) as connection:
        connection.request("GET", "/health")
        first = connection.getresponse()
        first.read()
        first_socket = connection.sock
        connection.request("GET", "/feed")
        second = connection.getresponse()
        second.read()
        if (
            first.status != 200
            or second.status != 200
            or first_socket is None
            or connection.sock is not first_socket
        ):
            raise RuntimeError("keep-alive check failed")
    print("42 official checks passed; keep-alive connection reused.")


@dataclass(frozen=True)
class LoadGenerator:
    invocation: list[str]
    api_port: int
    version: str
    sha256: str
    container: str | None = None

    def sample(self, pid: int) -> dict[str, JSONValue]:
        if self.container is None:
            client = psutil.Process(pid)
            cpu = client.cpu_times()

            return {
                "client_cpu_s": cpu.user + cpu.system,
                "client_rss_bytes": client.memory_info().rss,
            }
        values = command(
            "docker",
            "exec",
            self.container,
            "sh",
            "-c",
            "cat /sys/fs/cgroup/cpu.stat; printf 'memory '; cat /sys/fs/cgroup/memory.current",
        )
        rows = {line.split()[0]: int(line.split()[1]) for line in values.splitlines()}

        return {"client_cpu_s": rows["usage_usec"] / 1e6, "client_cgroup_bytes": rows["memory"]}


@contextmanager
def load_generator(
    args: Arguments, container: str, port: int, output: Path
) -> Generator[LoadGenerator]:
    name: str | None = None
    if args.k6 is not None:
        k6 = args.k6.resolve()
        prefix = [str(k6)]
        version = command(str(k6), "version")
        sha256 = file_hash(k6)
        with socket.socket() as address:
            address.bind(("127.0.0.1", 0))
            api_port = address.getsockname()[1]
        listen = f"127.0.0.1:{api_port}"
        target = f"http://127.0.0.1:{port}"
        summary = str(output / "k6-summary.json")
        tokens = (ROOT / "seed/tokens.json").as_posix()
        script = str(ROOT / "bench/load.js")
    else:
        name = f"{container}-load"
        environment = mapping(read_json(output / "environment.json"))
        vm_memory = integer(mapping(environment["docker_info"]), "MemTotal")
        # Leave space for the API and Docker; contain generator failures in its own cgroup.
        client_memory = max(vm_memory // 2, vm_memory - 4 * 2**30)
        api_port = int(command("docker", "port", container, "6565/tcp").rsplit(":", 1)[1])
        prefix = [
            "docker",
            "run",
            f"--name={name}",
            "--cap-drop=ALL",
            f"--memory={client_memory}",
            f"--memory-swap={client_memory}",
            f"--env=GOMEMLIMIT={client_memory * 9 // 10}B",
            f"--network=container:{container}",
            "--ulimit=nofile=1048576:1048576",
            f"--mount=type=bind,source={ROOT},target=/work,readonly",
            f"--mount=type=bind,source={output},target=/results",
            K6_IMAGE,
        ]
        version = command("docker", "run", "--rm", "--network=none", K6_IMAGE, "version")
        sha256 = command(
            "docker",
            "run",
            "--rm",
            "--network=none",
            "--entrypoint=sha256sum",
            K6_IMAGE,
            "/usr/bin/k6",
        ).split()[0]
        listen = "0.0.0.0:6565"
        target = "http://127.0.0.1:3000"
        summary, tokens, script = (
            "/results/k6-summary.json",
            "/work/seed/tokens.json",
            "/work/bench/load.js",
        )
    invocation = [
        *prefix,
        "run",
        "--quiet",
        "--paused",
        "--address",
        listen,
        *(["--local-ips=127.0.0.2-127.0.0.33"] if name is not None else []),
        "--summary-export",
        summary,
        "-e",
        f"BASE_URL={target}",
        "-e",
        f"VUS={args.vus}",
        "-e",
        f"DURATION={args.hold}",
        "-e",
        f"RAMP_UP={args.ramp}",
        "-e",
        f"TOKENS={tokens}",
        script,
    ]
    try:
        yield LoadGenerator(invocation, api_port, version, sha256, name)
    finally:
        if name is not None:
            inspected = subprocess.run(["docker", "inspect", name], capture_output=True, text=True)
            (output / "load-generator.json").write_text(inspected.stdout, encoding="utf-8")
            subprocess.run(["docker", "rm", "--force", name], capture_output=True, check=False)


def load(args: Arguments, generator: LoadGenerator, observer: str, output: Path) -> int:
    warmup = duration_seconds(args.ramp)
    hold = duration_seconds(args.hold)
    profiling = args.action == "profile"
    ticks = int(command("docker", "exec", observer, "getconf", "CLK_TCK"))
    samples: list[dict[str, JSONValue]] = []
    with (output / "k6.log").open("w", encoding="utf-8") as log:
        with subprocess.Popen(generator.invocation, stdout=log, stderr=subprocess.STDOUT) as child:
            try:
                initializing = time.monotonic()
                ready_deadline = initializing + 120
                next_init_sample = next_init_progress = initializing
                while True:
                    if child.poll() is not None:
                        raise RuntimeError("k6 exited during initialization; inspect k6.log")
                    try:
                        status = k6_status(generator.api_port)
                        if status.get("status") == K6_PAUSED_BEFORE_RUN:
                            break
                    except OSError, http.client.HTTPException:
                        pass
                    now = time.monotonic()
                    if now >= next_init_sample:
                        try:
                            samples.append(
                                {
                                    "phase": "initializing",
                                    "elapsed_s": now - initializing,
                                    **generator.sample(child.pid),
                                }
                            )
                        except psutil.NoSuchProcess, subprocess.CalledProcessError:
                            if child.poll() is not None:
                                raise RuntimeError(
                                    "k6 exited during initialization; inspect k6.log"
                                ) from None
                        next_init_sample = now + 2
                    if now >= next_init_progress:
                        print(
                            f"{args.label}: initializing k6 ({now - initializing:.0f}s)", flush=True
                        )
                        next_init_progress = now + 30
                    if time.monotonic() >= ready_deadline:
                        raise RuntimeError("k6 did not initialize within 120 seconds")
                    time.sleep(0.1)
                before = counters(observer)
                started = time.monotonic()
                k6_status(generator.api_port, resume=True)
                resumed = time.monotonic() - started
                next_sample = next_progress = 0.0
                window: dict[str, JSONValue] = {
                    "warmup_s": warmup,
                    "hold_s": hold,
                    "resume_ack_s": resumed,
                }
                profile_before: dict[str, JSONValue] | None = None
                profile_done = False
                while child.poll() is None:
                    elapsed = time.monotonic() - started
                    if profiling and profile_before is None and elapsed >= warmup:
                        profile_before = counters(observer)
                        window["enable_request_s"] = time.monotonic() - started
                        perf_control(observer, "enable")
                        window["enable_ack_s"] = time.monotonic() - started
                        print(f"{args.label}: warm-up complete; sampling {args.hold}", flush=True)
                    if profiling and not profile_done and elapsed >= warmup + hold:
                        window["disable_request_s"] = time.monotonic() - started
                        perf_control(observer, "disable")
                        window["disable_ack_s"] = time.monotonic() - started
                        profile_after = counters(observer)
                        if profile_before is None:
                            raise RuntimeError("profile window never started")
                        window["cpu"] = dict(cpu_delta(profile_before, profile_after, ticks))
                        window["cgroup_before"] = profile_before
                        window["cgroup_after"] = profile_after
                        write_json(output / "window.json", window)
                        profile_done = True
                        print(f"{args.label}: sampling complete; draining workload", flush=True)
                    if elapsed < next_sample:
                        time.sleep(0.1)
                        continue
                    next_sample = elapsed + 2
                    try:
                        sample: dict[str, JSONValue] = {
                            "phase": "workload",
                            "elapsed_s": time.monotonic() - started,
                            **generator.sample(child.pid),
                            "server": counters(observer),
                        }
                        samples.append(sample)
                        if elapsed >= next_progress:
                            next_progress = elapsed + 30
                            print(
                                f"{args.label}: {time.monotonic() - started:.0f}s elapsed",
                                flush=True,
                            )
                    except (psutil.NoSuchProcess, subprocess.CalledProcessError) as error:
                        try:
                            child.wait(timeout=15)
                        except subprocess.TimeoutExpired:
                            raise error from None
                        break
                code = child.wait(timeout=15)
                if profiling and not profile_done:
                    raise RuntimeError("load ended before the profile window completed")
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait(timeout=15)
                write_json(output / "samples.json", samples)
    after = counters(observer)
    write_json(
        output / "run.json",
        {
            "arguments": generator.invocation,
            "exit_code": code,
            "wall_seconds": time.monotonic() - started,
            "cgroup_before": before,
            "cgroup_after": after,
            "cpu_scope": "server_cgroup",
            "clock_ticks_per_second": ticks,
            "initial_k6_status": status,
            "cpu": cpu_delta(before, after, ticks),
            "load_generator": "docker_loopback" if generator.container is not None else "native",
            "load_generator_version": generator.version,
            "load_generator_sha256": generator.sha256,
        },
    )
    print(f"k6 exit={code}; results={output}", flush=True)
    if code not in (0, 99):
        raise RuntimeError("load generator aborted or failed; inspect k6.log")
    return code


@contextmanager
def server(
    output: Path, *, database: Path | None = None, secret: str = JWT_KEY
) -> Generator[tuple[str, int]]:
    unique = f"td-bench-{uuid.uuid4().hex[:10]}"
    volume = unique + "-data"
    seed = (database or ROOT / "seed/feed.db").resolve()
    if database is None and not seed.is_file():
        raise ValueError("seed/feed.db is missing")
    launch = "exec /app/server"
    if seed.is_file():
        launch = f"cp {shlex.quote('/seed/' + seed.name)} /data/feed.db && {launch}"
    command("docker", "volume", "create", volume)
    try:
        command(
            "docker",
            "run",
            "--rm",
            "--user",
            "0",
            "--volume",
            f"{volume}:/data",
            "--entrypoint",
            "chown",
            IMAGE,
            "10001:10001",
            "/data",
        )
        command(
            "docker",
            "run",
            "--detach",
            "--name",
            unique,
            "--cpus",
            "1",
            "--memory",
            "2g",
            "--memory-swap",
            "2g",
            "--cap-drop",
            "ALL",
            "--security-opt",
            f"seccomp={ROOT / 'src/docker/seccomp.json'}",
            "--ulimit",
            "nofile=1048576:1048576",
            "--env",
            f"JWT_SECRET={secret}",
            "--publish",
            "127.0.0.1::3000",
            "--publish",
            "127.0.0.1::6565",
            "--volume",
            f"{volume}:/data",
            "--mount",
            f"type=bind,source={seed.parent},target=/seed,readonly",
            "--entrypoint",
            "sh",
            IMAGE,
            "-c",
            launch,
        )
        port = int(command("docker", "port", unique, "3000/tcp").rsplit(":", 1)[1])
        for _ in range(120):
            if healthy(port, expected=200 if seed.is_file() else 503):
                break
            time.sleep(0.5)
        else:
            raise RuntimeError("container did not become healthy")
        write_json(
            output / "environment.json",
            {
                "host": platform.platform(),
                "logical_cpus": psutil.cpu_count(),
                "host_memory_bytes": psutil.virtual_memory().total,
                "docker_info": decode_json(
                    command("docker", "info", "--format", "{{json .}}").encode()
                ),
                "container": decode_json(command("docker", "inspect", unique).encode()),
                "image": decode_json(command("docker", "image", "inspect", IMAGE).encode()),
                "runtime": decode_json(
                    command("docker", "exec", unique, "/app/server", "--version").encode()
                ),
                "filesystem": command("docker", "exec", unique, "df", "-T", "/data"),
                "limits": command(
                    "docker",
                    "exec",
                    unique,
                    "sh",
                    "-c",
                    "ulimit -n; cat /sys/fs/cgroup/cpu.max /sys/fs/cgroup/memory.max /sys/fs/cgroup/memory.swap.max",
                ),
                "git_commit": command("git", "-C", str(ROOT), "rev-parse", "HEAD"),
                "git_status": command("git", "-C", str(ROOT), "status", "--short"),
                "workload_sha256": hashlib.sha256(
                    (ROOT / "bench/load.js").read_bytes()
                ).hexdigest(),
                "seed_sha256": file_hash(seed) if seed.is_file() else None,
            },
        )
        yield unique, port
    finally:
        state = subprocess.run(["docker", "inspect", unique], capture_output=True, text=True)
        (output / "container-final.json").write_text(state.stdout, encoding="utf-8")
        logs = subprocess.run(["docker", "logs", unique], capture_output=True, text=True)
        (output / "server.log").write_text(logs.stdout + logs.stderr, encoding="utf-8")
        subprocess.run(["docker", "rm", "--force", unique], capture_output=True, check=False)
        command("docker", "volume", "rm", volume)


def profile_stage(frames: list[tuple[str, str]]) -> str:
    symbols = "\n".join(symbol for symbol, _ in frames)
    if "tds::authenticate(" in symbols or any("libcrypto" in module for _, module in frames):
        return "auth"
    if "tds::quote(" in symbols or "tds::integer(" in symbols:
        return "response_json"
    if "tds::read_json(" in symbols or "yyjson" in symbols:
        return "request_json"
    if "sqlite3" in symbols or "tds::Database::" in symbols:
        return "database_and_rows"
    if any(name in symbols for name in ("tds::Server::", "tds::Api::", "llhttp", "io_uring")):
        return "transport_and_other"
    return "unknown"


def analyze_profile(stacks: str, statistics: str) -> NativeProfile:
    stages: Counter[str] = Counter()
    weights: Counter[tuple[str, str]] = Counter()
    counts: Counter[tuple[str, str]] = Counter()
    samples = unresolved = unrooted = 0
    for block in re.split(r"\n\s*\n", stacks.strip()):
        lines = block.splitlines()
        if len(lines) < 2 or not lines[0].strip().isdigit():
            raise ValueError("invalid perf stack header")
        period = int(lines[0].strip())
        if period <= 0:
            raise ValueError("perf sample period must be positive")
        frames: list[tuple[str, str]] = []
        for line in lines[1:]:
            frame = re.fullmatch(r"\s*[0-9a-f]+\s+(.+) \((.+)\)\s*", line)
            if frame is None:
                raise ValueError(f"invalid perf frame: {line}")
            frames.append((frame[1], frame[2]))
        leaf = frames[0]
        samples += 1
        unresolved += leaf[0] == "[unknown]"
        unrooted += not any(symbol == "main" for symbol, _ in frames)
        stages[profile_stage(frames)] += period
        weights[leaf] += period
        counts[leaf] += 1

    aggregate = statistics.split(f"{PROFILE_EVENT} stats:", 1)[0]
    recorded = re.search(r"^\s*SAMPLE events:\s*(\d+)", aggregate, re.MULTILINE)
    if recorded is None or int(recorded[1]) != samples:
        raise ValueError("parsed stacks do not match perf's recorded sample count")
    lost = sum(
        int(value)
        for value in re.findall(r"^\s*LOST(?:_SAMPLES)? events:\s*(\d+)", aggregate, re.MULTILINE)
    )
    if lost:
        raise ValueError(
            f"perf reported {lost} loss records; retain the raw data and lower the rate"
        )
    return NativeProfile(
        event=PROFILE_EVENT,
        frequency_hz=PROFILE_FREQUENCY,
        stack_bytes=PROFILE_STACK_BYTES,
        samples=samples,
        sampled_cpu_ns=sum(stages.values()),
        lost_records=lost,
        unresolved_leaf_samples=unresolved,
        unrooted_samples=unrooted,
        stages_cpu_ns=dict(stages),
        hotspots=[
            Hotspot(symbol=key[0], module=key[1], cpu_ns=value, samples=counts[key])
            for key, value in weights.most_common(15)
        ],
    )


def show_profile(profile: dict[str, JSONValue]) -> None:
    total = number(profile, "sampled_cpu_ns")
    if total <= 0:
        raise ValueError("profile contains no sampled CPU time")
    print(
        f"User CPU: {integer(profile, 'samples'):,} samples; "
        f"{integer(profile, 'unresolved_leaf_samples'):,} unresolved leaves; "
        f"{integer(profile, 'unrooted_samples'):,} stacks without main."
    )
    values = mapping(profile["stages_cpu_ns"])
    stages = [(stage, number(values, stage)) for stage in values]
    for stage, weight in sorted(stages, key=lambda item: item[1], reverse=True):
        print(f"  {PROFILE_STAGES.get(stage, stage):24} {weight / total:6.1%}")
    hotspots = profile["hotspots"]
    if not isinstance(hotspots, list):
        raise ValueError("profile hotspots must be an array")
    print("Top functions:")
    for value in hotspots[:8]:
        row = mapping(value)
        print(f"  {number(row, 'cpu_ns') / total:6.1%}  {text(row, 'symbol')[:100]}")


def collect_profile(container: str, output: Path) -> None:
    common = [
        "docker",
        "run",
        "--rm",
        "--network=none",
        f"--pid=container:{container}",
        "--cap-drop=ALL",
        f"--mount=type=bind,source={output},target=/profile,readonly",
        PROFILER_IMAGE,
    ]
    stacks = command(
        *common,
        "script",
        "--symfs=/proc/1/root",
        "--input=/profile/perf.data",
        "-F",
        "period,ip,sym,dso",
    )
    statistics = command(
        *common,
        "report",
        "--symfs=/proc/1/root",
        "--input=/profile/perf.data",
        "--stdio",
        "--stats",
        "--no-children",
    )
    (output / "stacks.txt").write_text(stacks + "\n", encoding="utf-8")
    (output / "perf-stats.txt").write_text(statistics + "\n", encoding="utf-8")
    write_json(output / "profile.json", analyze_profile(stacks, statistics))


@contextmanager
def observing(container: str, output: Path, *, profiling: bool) -> Generator[str]:
    name = f"{container}-observer"
    launch = ["--read-only", "--entrypoint=sleep", IMAGE, "infinity"]
    target_hash = ""
    if profiling:
        target_hash = command("docker", "exec", container, "sha256sum", "/app/server").split()[0]
        symbol_hash = command(
            "docker", "run", "--rm", "--entrypoint=sha256sum", PROFILER_IMAGE, "/app/server"
        ).split()[0]
        if target_hash != symbol_hash:
            raise RuntimeError("profiler symbols do not match the running server")
        launch = [
            "--cap-add=PERFMON",
            "--entrypoint=sh",
            PROFILER_IMAGE,
            "-c",
            'mkfifo /tmp/perf-control /tmp/perf-ack && exec perf "$@"',
            "perf",
            "record",
            "--delay=-1",
            "--control=fifo:/tmp/perf-control,/tmp/perf-ack",
            f"--event={PROFILE_EVENT}",
            f"--freq={PROFILE_FREQUENCY}",
            f"--call-graph=dwarf,{PROFILE_STACK_BYTES}",
            "--pid=1",
            "--output=/tmp/perf.data",
        ]
    created = False
    completed = False
    try:
        command(
            "docker",
            "run",
            "--detach",
            f"--name={name}",
            "--network=none",
            f"--pid=container:{container}",
            "--user=10001:10001",
            "--cap-drop=ALL",
            "--cpus=1",
            "--memory=512m",
            "--memory-swap=512m",
            *launch,
        )
        created = True
        if profiling:
            time.sleep(1)
            if command("docker", "inspect", "--format", "{{.State.Running}}", name) != "true":
                raise RuntimeError(command("docker", "logs", name))
            perf_control(name, "ping")
            write_json(
                output / "sampler.json",
                {
                    "perf_version": command("docker", "exec", name, "perf", "--version"),
                    "binary_sha256": target_hash,
                    "container": decode_json(command("docker", "inspect", name).encode()),
                },
            )
        yield name
        completed = True
    finally:
        try:
            if created and profiling:
                command("docker", "stop", "--signal", "SIGINT", "--timeout", "15", name)
                logs = subprocess.run(["docker", "logs", name], capture_output=True, text=True)
                (output / "perf.log").write_text(logs.stdout + logs.stderr, encoding="utf-8")
                exit_code = command("docker", "inspect", "--format", "{{.State.ExitCode}}", name)
                if exit_code not in ("0", "130"):
                    raise RuntimeError(f"perf exited with {exit_code}; see perf.log")
                command("docker", "cp", f"{name}:/tmp/perf.data", str(output / "perf.data"))
                if completed:
                    collect_profile(container, output)
        finally:
            subprocess.run(["docker", "rm", "--force", name], capture_output=True, check=False)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    checks = actions.add_parser("check", help="lint, formatting and strict typing")
    checks.add_argument(
        "--docker", action="store_true", help="also build Linux and run native and HTTP tests"
    )
    for action, description in (
        ("bench", "run the official k6 workload in Docker"),
        ("profile", "sample native user CPU stacks during the workload"),
    ):
        benchmark = actions.add_parser(action, help=description)
        benchmark.add_argument("--label", required=True, help="unique lowercase run name")
        benchmark.add_argument("--vus", type=int, default=5000)
        benchmark.add_argument(
            "--hold", default="60s", help="time at target users (default: 60s; confirmation: 5m)"
        )
        benchmark.add_argument(
            "--ramp", default="30s", help="ramp-up time (default: 30s; confirmation: 60s)"
        )
        benchmark.add_argument(
            "--k6", type=Path, help="native k6 executable; defaults to pinned Linux k6 in Docker"
        )
        benchmark.add_argument("--save", type=Path, default=RESULTS if action == "bench" else None)
        benchmark.add_argument(
            "--no-build", action="store_true", help="reuse existing images without rebuilding"
        )
    results = actions.add_parser("results", help="display saved benchmark results")
    results.add_argument("file", type=Path, nargs="?", default=RESULTS)
    results.add_argument(
        "--json", action="store_true", dest="json_output", help="emit JSON for other tools"
    )
    args = parser.parse_args(namespace=Arguments())
    try:
        if args.action == "check":
            check(args.docker)
        elif args.action == "results":
            document = read_json(args.file)
            if args.json_output:
                print(json.dumps(document, allow_nan=False))
            elif isinstance(document, list):
                show_results(read_results(args.file))
            else:
                show_profile(mapping(mapping(document)["profile"]))
        else:
            if args.k6 is not None and not args.k6.is_file():
                parser.error("--k6 must point to an executable file")
            if (
                args.vus < 1
                or not args.label
                or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in args.label)
            ):
                parser.error("positive VUs and a lowercase alphanumeric/hyphen label are required")
            profiling = args.action == "profile"
            duration_seconds(args.ramp)
            duration_seconds(args.hold)
            destination = args.save
            saved = (
                read_results(destination)
                if not profiling and destination is not None and destination.exists()
                else []
            )
            if any(row["name"] == args.label for row in saved):
                parser.error("run name already exists; choose a new --label")
            output = ROOT / "results" / args.label
            output.mkdir(parents=True, exist_ok=False)
            if not args.no_build:
                build()
                if profiling:
                    build(profiler=True)
            with server(output) as (container, port):
                with (
                    observing(container, output, profiling=profiling) as name,
                    load_generator(args, container, port, output) as generator,
                ):
                    code = load(args, generator, name, output)
            result = summarize(output)
            show_results([result])
            if profiling:
                profile = mapping(read_json(output / "profile.json"))
                window = mapping(read_json(output / "window.json"))
                report = {
                    "benchmark": result,
                    "profile": profile,
                    "window": {
                        key: value for key, value in window.items() if not key.startswith("cgroup_")
                    },
                }
                write_json(output / "profile.json", report)
                if destination is not None:
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    write_json(destination, report)
                show_profile(profile)
            else:
                if destination is None:
                    raise ValueError("a result file is required for benchmarks")
                destination.parent.mkdir(parents=True, exist_ok=True)
                write_json(destination, [*saved, result])
            raise SystemExit(code)
    except subprocess.CalledProcessError as error:
        if error.stdout:
            print(error.stdout)
        if error.stderr:
            print(error.stderr)
        raise SystemExit(error.returncode) from error
    except (RuntimeError, ValueError, KeyError, OSError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
