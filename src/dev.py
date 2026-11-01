"""Checks, Docker benchmarks and saved results. Run with uv."""

import argparse
import hashlib
import http.client
import json
import math
import platform
import shutil
import subprocess
import tempfile
import time
import uuid
from collections.abc import Generator
from contextlib import closing, contextmanager
from datetime import UTC, datetime
from pathlib import Path
from typing import TypedDict

import psutil

from api.codec import JSONValue, decode_json

ROOT = Path(__file__).resolve().parent.parent
IMAGE = "twelve-dollar-baseline:local"
RESULTS = ROOT / "src/benchmarks/results.json"


class Arguments(argparse.Namespace):
    action: str
    docker: bool
    label: str
    vus: int
    hold: str
    ramp: str
    k6: Path | None
    save: Path
    file: Path
    json_output: bool


class Result(TypedDict):
    date: str
    name: str
    python: str
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


def command(*args: str) -> str:
    result = subprocess.run(args, check=True, capture_output=True, text=True, encoding="utf-8")
    return result.stdout.strip()


def write_json(path: Path, value: object) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
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


def healthy(port: int) -> bool:
    try:
        with closing(http.client.HTTPConnection("127.0.0.1", port, timeout=2)) as connection:
            connection.request("GET", "/health")
            response = connection.getresponse()
            response.read()
            return response.status == 200
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
                python=text(row, "python"),
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
    python_version, sqlite_version = text(environment, "runtime").split()
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
        python=python_version,
        sqlite=sqlite_version,
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
            "Python",
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
                row["python"],
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


def build() -> None:
    print("Building Ubuntu image...", flush=True)
    command(
        "docker",
        "build",
        "--quiet",
        "--platform",
        "linux/amd64",
        "--tag",
        IMAGE,
        "--file",
        str(ROOT / "src/Dockerfile"),
        str(ROOT / "src"),
    )


def check(use_docker: bool) -> None:
    for args in [
        ["ruff", "check", "."],
        ["ruff", "format", "--check", "."],
        ["pyright"],
        ["python", "-m", "unittest", "discover", "-v", "-s", "checks", "-p", "test_*.py"],
    ]:
        subprocess.run(["uv", "run", "--locked", *args], cwd=ROOT / "src", check=True)
    if use_docker:
        build()
        output = ROOT / "results"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="check-", dir=output) as directory:
            work = Path(directory).resolve()
            if not work.is_relative_to(output.resolve()):
                raise RuntimeError("check directory must stay under results/")
            with server(work) as (container, port):
                verify(container, port, work)


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
    aborted = False
    with (output / "k6.log").open("w", encoding="utf-8") as log:
        with subprocess.Popen(invocation, stdout=log, stderr=subprocess.STDOUT) as child:
            client = psutil.Process(child.pid)
            try:
                while child.poll() is None:
                    try:
                        cpu = client.cpu_times()
                        host = psutil.virtual_memory()
                        sample: dict[str, JSONValue] = {
                            "elapsed_s": time.monotonic() - started,
                            "client_cpu_s": cpu.user + cpu.system,
                            "client_rss_bytes": client.memory_info().rss,
                            "host_memory_percent": host.percent,
                            "host_available_bytes": host.available,
                            "host_cpu_percent": psutil.cpu_percent(),
                            "server": counters(container),
                        }
                        samples.append(sample)
                        if len(samples) % 15 == 0:
                            print(
                                f"{args.label}: {time.monotonic() - started:.0f}s, "
                                f"client RSS {client.memory_info().rss / 2**20:.0f} MiB, "
                                f"host memory {host.percent:.1f}%",
                                flush=True,
                            )
                        if host.percent >= 90:
                            aborted = True
                            child.terminate()
                            break
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
            "host_memory_abort": aborted,
            "wall_seconds": time.monotonic() - started,
            "cgroup_before": before,
            "cgroup_after": after,
            "load_generator_version": command(str(k6), "version"),
            "load_generator_sha256": file_hash(k6),
        },
    )
    print(f"k6 exit={code}; memory_abort={aborted}; results={output}", flush=True)
    if aborted or code not in (0, 99):
        raise RuntimeError("load generator aborted or failed; inspect k6.log")
    return code


@contextmanager
def server(output: Path) -> Generator[tuple[str, int]]:
    unique = f"td-bench-{uuid.uuid4().hex[:10]}"
    volume = unique + "-data"
    seed = (ROOT / "seed/feed.db").resolve()
    if not seed.is_file():
        raise ValueError("seed/feed.db is missing")
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
            "--ulimit",
            "nofile=65535:65535",
            "--env",
            "JWT_SECRET=twelve-dollar-challenge",
            "--publish",
            "127.0.0.1::3000",
            "--volume",
            f"{volume}:/data",
            "--mount",
            f"type=bind,source={seed.parent},target=/seed,readonly",
            IMAGE,
            "sh",
            "-c",
            "cp /seed/feed.db /data/feed.db && exec bash start.sh",
        )
        port = int(command("docker", "port", unique, "3000/tcp").rsplit(":", 1)[1])
        for _ in range(120):
            if healthy(port):
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
                "runtime": command(
                    "docker",
                    "exec",
                    unique,
                    "uv",
                    "run",
                    "--locked",
                    "--no-sync",
                    "--no-dev",
                    "python",
                    "-c",
                    "import platform,sqlite3; print(platform.python_version(),sqlite3.sqlite_version)",
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
                "seed_sha256": file_hash(seed),
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    checks = actions.add_parser("check", help="lint, formatting, strict typing and unit tests")
    checks.add_argument(
        "--docker", action="store_true", help="also build Ubuntu and run official HTTP checks"
    )
    benchmark = actions.add_parser("bench", help="run the official k6 workload in Docker")
    benchmark.add_argument("--label", required=True, help="unique lowercase run name")
    benchmark.add_argument("--vus", type=int, default=5000)
    benchmark.add_argument("--hold", default="5m")
    benchmark.add_argument("--ramp", default="60s")
    benchmark.add_argument(
        "--k6", type=Path, default=shutil.which("k6"), help="k6 executable; defaults to PATH"
    )
    benchmark.add_argument("--save", type=Path, default=RESULTS, help="compact result file")
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
            saved = read_results(args.file)
            if args.json_output:
                print(json.dumps(saved))
            else:
                show_results(saved)
        else:
            if args.k6 is None or not args.k6.is_file():
                parser.error("install k6 or pass --k6 PATH")
            if (
                args.vus < 1
                or not args.label
                or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in args.label)
            ):
                parser.error("positive VUs and a lowercase alphanumeric/hyphen label are required")
            saved = read_results(args.save) if args.save.exists() else []
            if any(row["name"] == args.label for row in saved):
                parser.error("run name already exists; choose a new --label")
            output = ROOT / "results" / args.label
            output.mkdir(parents=True, exist_ok=False)
            build()
            with server(output) as (container, port):
                code = load(args, container, port, output)
            result = summarize(output)
            args.save.parent.mkdir(parents=True, exist_ok=True)
            write_json(args.save, [*saved, result])
            show_results([result])
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
