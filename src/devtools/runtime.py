"""Docker builds, workload execution and owned runtime resources"""

import hashlib
import http.client
import platform
import shlex
import socket
import subprocess
import time
import uuid
from collections.abc import Generator
from contextlib import closing, contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

import psutil

from . import reports

ROOT = Path(__file__).resolve().parents[2]
IMAGE = "twelve-dollar-cpp:local"
PROFILER_IMAGE = "twelve-dollar-profiler:local"
GENERATOR_IMAGE = "twelve-dollar-generator:local"
GENERATOR_SOURCE_LABEL = "org.twelve-dollar.generator-source-sha256"
K6_IMAGE = (
    "grafana/k6:1.3.0@sha256:a90b459a3768c46ad1013da53af24189f735d7112273c6ac3212ca8ed0e18656"
)
K6_PAUSED_BEFORE_RUN = 4
JWT_KEY = "twelve-dollar-challenge"
type GeneratorKind = Literal["cpp", "k6"]


@dataclass(frozen=True)
class RunConfig:
    label: str
    vus: int
    ramp: str
    hold: str
    mode: Literal["bench", "profile"] = "bench"
    generator: GeneratorKind = "cpp"
    k6: Path | None = None
    telemetry: reports.TelemetryMode = "off"
    scope: reports.ProfileScope = "user"
    seed: int | None = None


def command(*args: str, timeout: int | None = 30) -> str:
    result = subprocess.run(
        args, check=True, capture_output=True, text=True, encoding="utf-8", timeout=timeout
    )
    return result.stdout.strip()


def file_hash(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def counters(observer: str) -> dict[str, reports.JSONValue]:
    # Read through the server's root, but run the reader in a separate cgroup
    root = "/proc/1/root/sys/fs/cgroup"
    pressure = "".join(
        f"sed 's/^/{resource}_pressure /' {root}/{resource}.pressure; "
        for resource in ("io", "memory", "cpu")
    )
    values = command(
        "docker",
        "exec",
        observer,
        "sh",
        "-c",
        f"cat {root}/cpu.stat; "
        f"printf 'memory_current '; cat {root}/memory.current; "
        f"printf 'memory_peak '; cat {root}/memory.peak; "
        f"sed 's/^/memory_stat_/' {root}/memory.stat; "
        f"cat {root}/memory.events; {pressure}"
        "grep -h '^Tcp' /proc/1/net/snmp /proc/1/net/netstat; "
        "if [ -f /proc/1/root/data/feed.db ]; then "
        "printf 'database_file_bytes '; stat -c %s /proc/1/root/data/feed.db; fi; "
        "if [ -f /proc/1/root/data/feed.db-wal ]; then "
        "printf 'wal_file_bytes '; stat -c %s /proc/1/root/data/feed.db-wal; fi; "
        "printf 'process_stat '; cat /proc/1/stat",
    )
    return reports.parse_counters(values)


def k6_status(port: int, *, resume: bool = False) -> dict[str, reports.JSONValue]:
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
        return reports.mapping(
            reports.mapping(reports.mapping(reports.decode_json(data))["data"])["attributes"]
        )


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


def image_name(*, profiler: bool = False, telemetry: reports.TelemetryMode = "off") -> str:
    base = PROFILER_IMAGE if profiler else IMAGE
    return base + ("" if telemetry == "off" else f"-{telemetry}")


def build(
    *, tests: bool = False, profiler: bool = False, telemetry: reports.TelemetryMode = "off"
) -> str:
    print("Building profiler image..." if profiler else "Building C++ image...", flush=True)
    image = image_name(profiler=profiler, telemetry=telemetry) + ("-build" if tests else "")
    command(
        "docker",
        "build",
        "--progress",
        "plain",
        "--platform",
        "linux/amd64",
        "--tag",
        image,
        "--build-arg",
        f"TDS_TELEMETRY={telemetry}",
        *(["--target", "profiler"] if profiler else ["--target", "test-build"] if tests else []),
        "--file",
        str(ROOT / "src/Dockerfile"),
        str(ROOT / "src"),
        timeout=None,
    )
    return image


def generator_fingerprint() -> str:
    source = ROOT / "src"
    inputs = [
        source / name for name in ("load/main.cpp", "CMakeLists.txt", "Dockerfile", ".dockerignore")
    ]
    inputs.extend((source / "cmake").glob("*.cmake"))
    inputs.extend((source / "tests").glob("CMakeLists.txt"))
    inputs.extend((source / "benchmarks").glob("CMakeLists.txt"))
    digest = hashlib.sha256()
    for path in sorted(inputs):
        digest.update(path.relative_to(source).as_posix().encode() + b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def ensure_generator() -> None:
    fingerprint = generator_fingerprint()
    try:
        current = command(
            "docker",
            "image",
            "inspect",
            "--format",
            '{{index .Config.Labels "' + GENERATOR_SOURCE_LABEL + '"}}',
            GENERATOR_IMAGE,
        )
    except subprocess.CalledProcessError:
        current = ""
    if current == fingerprint:
        return
    print("Building current native generator...", flush=True)
    command(
        "docker",
        "build",
        "--progress",
        "plain",
        "--platform",
        "linux/amd64",
        "--tag",
        GENERATOR_IMAGE,
        "--label",
        f"{GENERATOR_SOURCE_LABEL}={fingerprint}",
        "--target",
        "generator",
        "--file",
        str(ROOT / "src/Dockerfile"),
        str(ROOT / "src"),
        timeout=None,
    )


@dataclass(frozen=True)
class LoadGenerator:
    invocation: list[str]
    api_port: int
    version: str
    sha256: str
    container: str | None = None
    kind: GeneratorKind = "k6"

    def sample(self, pid: int) -> dict[str, reports.JSONValue]:
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
    config: RunConfig, container: str, port: int, output: Path
) -> Generator[LoadGenerator]:
    name: str | None = None
    kind: GeneratorKind = "k6" if config.k6 is not None else config.generator
    if config.k6 is not None:
        k6 = config.k6.resolve()
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
        environment = reports.mapping(reports.read_json(output / "environment.json"))
        vm_memory = reports.integer(reports.mapping(environment["docker_info"]), "MemTotal")
        # Leave space for the API and Docker; contain generator failures in its own cgroup
        client_memory = max(vm_memory // 2, vm_memory - 4 * 2**30)
        api_port = int(command("docker", "port", container, "6565/tcp").rsplit(":", 1)[1])
        prefix = [
            "docker",
            "run",
            f"--name={name}",
            "--cap-drop=ALL",
            f"--memory={client_memory}",
            f"--memory-swap={client_memory}",
            f"--network=container:{container}",
            "--ulimit=nofile=1048576:1048576",
            f"--mount=type=bind,source={ROOT},target=/work,readonly",
            f"--mount=type=bind,source={output},target=/results",
        ]
        if kind == "cpp":
            metadata = command(
                "docker",
                "run",
                "--rm",
                "--network=none",
                "--entrypoint=sh",
                GENERATOR_IMAGE,
                "-c",
                "/app/load_generator --version; sha256sum /app/load_generator",
            ).splitlines()
            version, sha256 = metadata[0], metadata[1].split()[0]
        else:
            prefix += [f"--env=GOMEMLIMIT={client_memory * 9 // 10}B", K6_IMAGE]
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
    settings = [
        "-e",
        f"BASE_URL={target}",
        "-e",
        f"VUS={config.vus}",
        "-e",
        f"DURATION={config.hold}",
        "-e",
        f"RAMP_UP={config.ramp}",
        "-e",
        f"TOKENS={tokens}",
    ]
    if kind == "cpp":
        invocation = [
            *prefix,
            *settings,
            "--entrypoint=/app/load_generator",
            GENERATOR_IMAGE,
            "--summary-export",
            summary,
            "--local-ips=127.0.0.2-127.0.0.33",
            *(["--seed", str(config.seed)] if config.seed is not None else []),
            "--ready-file",
            "/results/generator.ready",
            "--start-gate",
            "/results/generator.start",
            *(["--diagnostics"] if config.telemetry == "profile" else []),
        ]
    else:
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
            *settings,
            script,
        ]
    try:
        yield LoadGenerator(invocation, api_port, version, sha256, name, kind)
    finally:
        if name is not None:
            inspected = subprocess.run(["docker", "inspect", name], capture_output=True, text=True)
            (output / "load-generator.json").write_text(inspected.stdout, encoding="utf-8")
            subprocess.run(["docker", "rm", "--force", name], capture_output=True, check=False)


def load(config: RunConfig, generator: LoadGenerator, observer: str, output: Path) -> int:
    warmup = reports.duration_seconds(config.ramp)
    hold = reports.duration_seconds(config.hold)
    profiling = config.mode == "profile"
    ticks = int(command("docker", "exec", observer, "getconf", "CLK_TCK"))
    samples: list[dict[str, reports.JSONValue]] = []
    with (output / "load.log").open("w", encoding="utf-8") as log:
        with subprocess.Popen(generator.invocation, stdout=log, stderr=subprocess.STDOUT) as child:
            try:
                initializing = time.monotonic()
                ready_deadline = initializing + 120
                next_init_sample = next_init_progress = initializing
                while True:
                    if child.poll() is not None:
                        raise RuntimeError(
                            "generator exited during initialization; inspect load.log"
                        )
                    try:
                        if generator.kind == "cpp":
                            status: dict[str, reports.JSONValue] = {
                                "ready": (output / "generator.ready").is_file()
                            }
                        else:
                            status = k6_status(generator.api_port)
                        if (
                            status.get("ready") is True
                            or status.get("status") == K6_PAUSED_BEFORE_RUN
                        ):
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
                                    "generator exited during initialization; inspect load.log"
                                ) from None
                        next_init_sample = now + 2
                    if now >= next_init_progress:
                        print(
                            f"{config.label}: initializing {generator.kind} ({now - initializing:.0f}s)",
                            flush=True,
                        )
                        next_init_progress = now + 30
                    if time.monotonic() >= ready_deadline:
                        raise RuntimeError("generator did not initialize within 120 seconds")
                    time.sleep(0.1)
                before = counters(observer)
                started = time.monotonic()
                if generator.kind == "cpp":
                    (output / "generator.start").touch()
                else:
                    k6_status(generator.api_port, resume=True)
                resumed = time.monotonic() - started
                next_sample = next_progress = 0.0
                window: dict[str, reports.JSONValue] = {
                    "warmup_s": warmup,
                    "hold_s": hold,
                    "resume_ack_s": resumed,
                }
                profile_before: dict[str, reports.JSONValue] | None = None
                profile_done = False
                while child.poll() is None:
                    elapsed = time.monotonic() - started
                    if profiling and profile_before is None and elapsed >= warmup:
                        profile_before = counters(observer)
                        window["enable_request_s"] = time.monotonic() - started
                        perf_control(observer, "enable")
                        window["enable_ack_s"] = time.monotonic() - started
                        print(
                            f"{config.label}: warm-up complete; sampling {config.hold}", flush=True
                        )
                    if profiling and not profile_done and elapsed >= warmup + hold:
                        window["disable_request_s"] = time.monotonic() - started
                        perf_control(observer, "disable")
                        window["disable_ack_s"] = time.monotonic() - started
                        profile_after = counters(observer)
                        if profile_before is None:
                            raise RuntimeError("profile window never started")
                        window["cpu"] = dict(
                            reports.cpu_delta(profile_before, profile_after, ticks)
                        )
                        window["cgroup_before"] = profile_before
                        window["cgroup_after"] = profile_after
                        reports.write_json(output / "window.json", window)
                        profile_done = True
                        print(f"{config.label}: sampling complete; draining workload", flush=True)
                    if elapsed < next_sample:
                        time.sleep(0.1)
                        continue
                    next_sample = elapsed + 2
                    try:
                        client_started = time.monotonic() - started
                        client_sample = generator.sample(child.pid)
                        client_finished = time.monotonic() - started
                        server_started = time.monotonic() - started
                        server_sample = counters(observer)
                        server_finished = time.monotonic() - started
                        sample: dict[str, reports.JSONValue] = {
                            "phase": "workload",
                            "elapsed_s": client_started,
                            "client_sample_end_s": client_finished,
                            "server_sample_start_s": server_started,
                            "server_sample_end_s": server_finished,
                            **client_sample,
                            "server": server_sample,
                        }
                        samples.append(sample)
                        if elapsed >= next_progress:
                            next_progress = elapsed + 30
                            print(
                                f"{config.label}: {time.monotonic() - started:.0f}s elapsed",
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
                reports.write_json(output / "samples.json", samples)
    after = counters(observer)
    reports.write_json(
        output / "run.json",
        {
            "arguments": generator.invocation,
            "exit_code": code,
            "wall_seconds": time.monotonic() - started,
            "cgroup_before": before,
            "cgroup_after": after,
            "cpu_scope": "server_cgroup",
            "clock_ticks_per_second": ticks,
            "initial_generator_status": status,
            "cpu": reports.cpu_delta(before, after, ticks),
            "load_generator": "docker_loopback" if generator.container is not None else "native",
            "generator": generator.kind,
            "telemetry": config.telemetry,
            "load_generator_version": generator.version,
            "load_generator_sha256": generator.sha256,
        },
    )
    print(f"{generator.kind} exit={code}; results={output}", flush=True)
    if code not in (0, 99):
        raise RuntimeError("load generator aborted or failed; inspect load.log")
    return code


@contextmanager
def server(
    output: Path,
    *,
    database: Path | None = None,
    secret: str = JWT_KEY,
    image: str | None = None,
    telemetry: reports.TelemetryMode = "off",
) -> Generator[tuple[str, int]]:
    image = image or image_name(telemetry=telemetry)
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
            image,
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
            *(["--env", "TDS_TELEMETRY_PATH=/data/telemetry.jsonl"] if telemetry != "off" else []),
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
            image,
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
        version = reports.mapping(
            reports.decode_json(
                command("docker", "exec", unique, "/app/server", "--version").encode()
            )
        )
        actual_telemetry = version.get("telemetry")
        if actual_telemetry is None and telemetry == "off":
            print(
                "Reference image does not identify its telemetry mode; requested off.", flush=True
            )
        elif actual_telemetry != telemetry:
            raise RuntimeError(
                f"server telemetry mode is {actual_telemetry!r}, requested {telemetry!r}"
            )
        reports.write_json(
            output / "environment.json",
            {
                "host": platform.platform(),
                "logical_cpus": psutil.cpu_count(),
                "host_memory_bytes": psutil.virtual_memory().total,
                "docker_info": reports.decode_json(
                    command("docker", "info", "--format", "{{json .}}").encode()
                ),
                "container": reports.decode_json(command("docker", "inspect", unique).encode()),
                "image": reports.decode_json(command("docker", "image", "inspect", image).encode()),
                "runtime": version,
                "telemetry": telemetry,
                "telemetry_mode_verified": actual_telemetry is not None,
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
        try:
            subprocess.run(
                ["docker", "stop", "--time=10", unique],
                capture_output=True,
                check=False,
                timeout=20,
            )
        except subprocess.TimeoutExpired:
            print("Server stop timed out; removing the container.", flush=True)
        try:
            state = subprocess.run(["docker", "inspect", unique], capture_output=True, text=True)
            (output / "container-final.json").write_text(state.stdout, encoding="utf-8")
            logs = subprocess.run(["docker", "logs", unique], capture_output=True, text=True)
            (output / "server.log").write_text(logs.stdout + logs.stderr, encoding="utf-8")
            if telemetry != "off":
                collect_telemetry(unique, output, telemetry)
        finally:
            subprocess.run(["docker", "rm", "--force", unique], capture_output=True, check=False)
            command("docker", "volume", "rm", volume)


def collect_telemetry(container: str, output: Path, mode: reports.TelemetryMode) -> None:
    raw = output / "telemetry.jsonl"
    report = reports.parse_telemetry("", expected_mode=mode)
    try:
        copied = subprocess.run(
            ["docker", "cp", f"{container}:/data/telemetry.jsonl", str(raw)],
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
        if copied.returncode != 0:
            raise RuntimeError(copied.stderr.strip() or "server telemetry export is missing")
        report = reports.parse_telemetry(raw.read_text(encoding="utf-8"), expected_mode=mode)
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        report["collection_errors"].append(str(error))
    try:
        reports.write_json(output / "telemetry.json", report, compact=True)
    except OSError as error:
        print(f"Could not save server telemetry: {error}", flush=True)
        return
    if not report["complete"]:
        print(
            "Server telemetry is incomplete; inspect telemetry.json and telemetry.jsonl.",
            flush=True,
        )


def collect_profile(
    container: str, output: Path, *, scope: reports.ProfileScope = "user", image: str | None = None
) -> None:
    image = image or PROFILER_IMAGE
    common = [
        "docker",
        "run",
        "--rm",
        "--network=none",
        f"--pid=container:{container}",
        "--cap-drop=ALL",
        *(["--user=0", "--cap-add=SYSLOG", "--cap-add=SYS_PTRACE"] if scope != "user" else []),
        f"--mount=type=bind,source={output},target=/profile,readonly",
        image,
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
    reports.write_json(
        output / "profile.json", reports.analyze_profile(stacks, statistics, scope=scope)
    )


@contextmanager
def observing(
    container: str,
    output: Path,
    *,
    profiling: bool,
    scope: reports.ProfileScope = "user",
    image: str | None = None,
    profiler_image: str | None = None,
) -> Generator[str]:
    image = image or IMAGE
    profiler_image = profiler_image or PROFILER_IMAGE
    name = f"{container}-observer"
    launch = ["--read-only", "--entrypoint=sleep", image, "infinity"]
    target_hash = ""
    if profiling:
        target_hash = command("docker", "exec", container, "sha256sum", "/app/server").split()[0]
        symbol_hash = command(
            "docker", "run", "--rm", "--entrypoint=sha256sum", profiler_image, "/app/server"
        ).split()[0]
        if target_hash != symbol_hash:
            raise RuntimeError("profiler symbols do not match the running server")
        launch = [
            "--cap-add=PERFMON",
            *(["--cap-add=SYSLOG", "--cap-add=SYS_PTRACE"] if scope != "user" else []),
            "--entrypoint=sh",
            profiler_image,
            "-c",
            'mkfifo /tmp/perf-control /tmp/perf-ack && exec perf "$@"',
            "perf",
            "record",
            "--delay=-1",
            "--control=fifo:/tmp/perf-control,/tmp/perf-ack",
            f"--event={reports.profile_event(scope)}",
            f"--freq={reports.PROFILE_FREQUENCY}",
            "--call-graph=fp"
            if scope == "kernel"
            else f"--call-graph=dwarf,{reports.PROFILE_STACK_BYTES}",
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
            "--user=0" if profiling and scope != "user" else "--user=10001:10001",
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
            reports.write_json(
                output / "sampler.json",
                {
                    "perf_version": command("docker", "exec", name, "perf", "--version"),
                    "binary_sha256": target_hash,
                    "container": reports.decode_json(command("docker", "inspect", name).encode()),
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
                    collect_profile(container, output, scope=scope, image=profiler_image)
        finally:
            subprocess.run(["docker", "rm", "--force", name], capture_output=True, check=False)


def run_workload(config: RunConfig, output: Path, *, image: str) -> int:
    if config.k6 is None and config.generator == "cpp":
        ensure_generator()
    profiling = config.mode == "profile"
    with server(output, image=image, telemetry=config.telemetry) as (container, port):
        with (
            observing(
                container,
                output,
                profiling=profiling,
                scope=config.scope if profiling else "user",
                image=image,
                profiler_image=image_name(profiler=True, telemetry=config.telemetry),
            ) as observer,
            load_generator(config, container, port, output) as generator,
        ):
            return load(config, generator, observer, output)
