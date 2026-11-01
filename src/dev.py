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
import subprocess
import tarfile
import tempfile
import time
import uuid
from collections import Counter
from collections.abc import Generator
from contextlib import closing, contextmanager
from datetime import UTC, datetime
from pathlib import Path
from typing import Never, TypedDict, cast

import psutil

ROOT = Path(__file__).resolve().parent.parent
IMAGE = "twelve-dollar-cpp:local"
PROFILER_IMAGE = "twelve-dollar-profiler:local"
PROFILE_EVENT = "cpu-clock:u"
PROFILE_FREQUENCY = 997
PROFILE_STACK_BYTES = 8192
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


class Result(TypedDict):
    date: str
    name: str
    runtime: str
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


def command(*args: str) -> str:
    result = subprocess.run(args, check=True, capture_output=True, text=True, encoding="utf-8")
    return result.stdout.strip()


def write_json(path: Path, value: object) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def file_hash(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def counters(container: str) -> dict[str, JSONValue]:
    # cgroup counters include the server, its page cache and these short sampler processes.
    values = command(
        "docker",
        "exec",
        container,
        "sh",
        "-c",
        "cat /sys/fs/cgroup/cpu.stat; "
        "printf 'memory_current '; cat /sys/fs/cgroup/memory.current; "
        "printf 'memory_peak '; cat /sys/fs/cgroup/memory.peak; "
        "cat /sys/fs/cgroup/memory.events",
    )
    return {line.split()[0]: int(line.split()[1]) for line in values.splitlines()}


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
            "Users",
            "Hold",
            "Req/s",
            "p95 ms",
            "p99 ms",
            "Errors",
            "CPU us/req",
            "RAM MiB",
            "Pass",
        ]
    ]
    for row in results:
        rows.append(
            [
                f"{row['date']}/{row['name']}",
                row["runtime"],
                str(row["users"]),
                row["hold"],
                f"{row['rps']:.2f}",
                f"{row['p95_ms']:.3f}",
                f"{row['p99_ms']:.3f}",
                f"{row['error_rate']:.2%}",
                f"{row['cpu_us_per_request']:.2f}",
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


def load(args: Arguments, container: str, port: int, output: Path) -> int:
    if args.k6 is None:
        raise ValueError("k6 is required for load generation")
    k6 = args.k6.resolve()
    invocation = [
        str(k6),
        "run",
        "--quiet",
        "--summary-export",
        str(output / "k6-summary.json"),
        "-e",
        f"BASE_URL=http://127.0.0.1:{port}",
        "-e",
        f"VUS={args.vus}",
        "-e",
        f"DURATION={args.hold}",
        "-e",
        f"RAMP_UP={args.ramp}",
        "-e",
        f"TOKENS={(ROOT / 'seed/tokens.json').as_posix()}",
        str(ROOT / "bench/load.js"),
    ]
    before = counters(container)
    started = time.monotonic()
    samples: list[dict[str, JSONValue]] = []
    with (output / "k6.log").open("w", encoding="utf-8") as log:
        with subprocess.Popen(invocation, stdout=log, stderr=subprocess.STDOUT) as child:
            client = psutil.Process(child.pid)
            try:
                while child.poll() is None:
                    try:
                        cpu = client.cpu_times()
                        sample: dict[str, JSONValue] = {
                            "elapsed_s": time.monotonic() - started,
                            "client_cpu_s": cpu.user + cpu.system,
                            "client_rss_bytes": client.memory_info().rss,
                            "server": counters(container),
                        }
                        samples.append(sample)
                        if len(samples) % 15 == 0:
                            print(
                                f"{args.label}: {time.monotonic() - started:.0f}s elapsed",
                                flush=True,
                            )
                    except psutil.NoSuchProcess:
                        break
                    time.sleep(2)
                code = child.wait(timeout=15)
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait(timeout=15)
                write_json(output / "samples.json", samples)
    after = counters(container)
    write_json(
        output / "run.json",
        {
            "arguments": invocation,
            "exit_code": code,
            "wall_seconds": time.monotonic() - started,
            "cgroup_before": before,
            "cgroup_after": after,
            "load_generator_version": command(str(k6), "version"),
            "load_generator_sha256": file_hash(k6),
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
            "nofile=65535:65535",
            "--env",
            f"JWT_SECRET={secret}",
            "--publish",
            "127.0.0.1::3000",
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


@contextmanager
def sampling(container: str, output: Path) -> Generator[None]:
    target_hash = command("docker", "exec", container, "sha256sum", "/app/server").split()[0]
    symbol_hash = command(
        "docker", "run", "--rm", "--entrypoint", "sha256sum", PROFILER_IMAGE, "/app/server"
    ).split()[0]
    if target_hash != symbol_hash:
        raise RuntimeError("profiler symbols do not match the running server")
    name = f"{container}-perf"
    created = False
    try:
        command(
            "docker",
            "run",
            "--detach",
            "--name",
            name,
            "--network",
            "none",
            "--pid",
            f"container:{container}",
            "--user",
            "10001:10001",
            "--cap-drop",
            "ALL",
            "--cap-add",
            "PERFMON",
            "--cpus",
            "1",
            "--memory",
            "512m",
            "--memory-swap",
            "512m",
            PROFILER_IMAGE,
            "record",
            "-e",
            PROFILE_EVENT,
            "-F",
            str(PROFILE_FREQUENCY),
            "--call-graph",
            f"dwarf,{PROFILE_STACK_BYTES}",
            "-p",
            "1",
            "-o",
            "/tmp/perf.data",
        )
        created = True
        time.sleep(1)
        if command("docker", "inspect", "--format", "{{.State.Running}}", name) != "true":
            raise RuntimeError(command("docker", "logs", name))
        write_json(
            output / "sampler.json",
            {
                "perf_version": command("docker", "exec", name, "perf", "--version"),
                "binary_sha256": target_hash,
                "container": decode_json(command("docker", "inspect", name).encode()),
            },
        )
        yield
    finally:
        try:
            if created:
                command("docker", "stop", "--signal", "SIGINT", "--timeout", "15", name)
                logs = subprocess.run(["docker", "logs", name], capture_output=True, text=True)
                (output / "perf.log").write_text(logs.stdout + logs.stderr, encoding="utf-8")
                exit_code = command("docker", "inspect", "--format", "{{.State.ExitCode}}", name)
                if exit_code not in ("0", "130"):
                    raise RuntimeError(f"perf exited with {exit_code}; see perf.log")
                command("docker", "cp", f"{name}:/tmp/perf.data", str(output / "perf.data"))
                common = [
                    "docker",
                    "run",
                    "--rm",
                    "--network",
                    "none",
                    "--pid",
                    f"container:{container}",
                    "--cap-drop",
                    "ALL",
                    "--mount",
                    f"type=bind,source={output},target=/profile,readonly",
                    PROFILER_IMAGE,
                ]
                stacks = command(
                    *common,
                    "script",
                    "--symfs",
                    "/proc/1/root",
                    "-i",
                    "/profile/perf.data",
                    "-F",
                    "period,ip,sym,dso",
                )
                statistics = command(
                    *common,
                    "report",
                    "--symfs",
                    "/proc/1/root",
                    "--stdio",
                    "--stats",
                    "--no-children",
                    "-i",
                    "/profile/perf.data",
                )
                (output / "stacks.txt").write_text(stacks + "\n", encoding="utf-8")
                (output / "perf-stats.txt").write_text(statistics + "\n", encoding="utf-8")
                write_json(output / "profile.json", analyze_profile(stacks, statistics))
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
            "--k6", type=Path, default=shutil.which("k6"), help="k6 executable; defaults to PATH"
        )
        benchmark.add_argument("--save", type=Path, default=RESULTS if action == "bench" else None)
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
            if args.k6 is None or not args.k6.is_file():
                parser.error("install k6 or pass --k6 PATH")
            if (
                args.vus < 1
                or not args.label
                or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in args.label)
            ):
                parser.error("positive VUs and a lowercase alphanumeric/hyphen label are required")
            profiling = args.action == "profile"
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
            build()
            if profiling:
                build(profiler=True)
            with server(output) as (container, port):
                if profiling:
                    with sampling(container, output):
                        code = load(args, container, port, output)
                else:
                    code = load(args, container, port, output)
            result = summarize(output)
            show_results([result])
            if profiling:
                profile = mapping(read_json(output / "profile.json"))
                report = {"benchmark": result, "profile": profile}
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
    except (RuntimeError, ValueError, KeyError, OSError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
