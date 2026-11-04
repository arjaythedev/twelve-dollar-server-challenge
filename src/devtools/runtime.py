"""Docker builds, workload execution and owned runtime resources"""

import hashlib
import http.client
import platform
import re
import shlex
import subprocess
import sys
import time
import uuid
from collections.abc import Generator
from contextlib import closing, contextmanager
from dataclasses import dataclass
from functools import cached_property
from pathlib import Path
from typing import Literal, cast

import psutil

from . import images, reports, runs, sources

ROOT = Path(__file__).resolve().parents[2]
K6_PAUSED_BEFORE_RUN = 4
JWT_KEY = "twelve-dollar-challenge"
type GeneratorKind = Literal["cpp", "k6"]
type PerfMode = Literal["off", "user", "kernel", "all"]


@dataclass(frozen=True)
class RunConfig:
    label: str
    vus: int
    ramp: str
    hold: str
    generator: GeneratorKind = "cpp"
    telemetry: reports.TelemetryMode = "off"
    perf: PerfMode = "off"
    client_diagnostics: bool = False
    seed: int | None = None

    def __post_init__(self) -> None:
        if self.vus < 1:
            raise ValueError("VUs must be positive")
        if re.fullmatch(r"[a-z0-9-]{1,96}", self.label) is None:
            raise ValueError(
                "a lowercase alphanumeric/hyphen label of at most 96 characters is required"
            )
        if self.seed is not None and (not 0 <= self.seed < 2**64 or self.generator != "cpp"):
            raise ValueError("--seed requires the C++ generator and an unsigned 64-bit integer")
        if self.client_diagnostics and self.generator != "cpp":
            raise ValueError("--client-diagnostics requires the C++ generator")

    @cached_property
    def timing(self) -> tuple[float, float]:
        return reports.duration_seconds(self.ramp), reports.duration_seconds(self.hold)

    @property
    def workload(self) -> dict[str, reports.JSONValue]:
        return {
            "generator": self.generator,
            "vus": self.vus,
            "ramp": self.ramp,
            "hold": self.hold,
            "seed": self.seed,
        }


def command(*args: str, timeout: int | None = 30) -> str:
    result = subprocess.run(
        args, check=True, capture_output=True, text=True, encoding="utf-8", timeout=timeout
    )
    return result.stdout.strip()


def file_hash(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def container_command(container: str, *args: str) -> str:
    return command("docker", "exec", container, *args)


@contextmanager
def preserving_failure(output: Path, stage: str) -> Generator[None]:
    # Cleanup runs while the original exception is still active, so keep it in charge
    primary = sys.exception()
    try:
        yield
    except Exception as error:
        message = f"{stage}: {error}"
        path = output / "collection-errors.json"
        try:
            previous = reports.read_json(path) if path.is_file() else []
            errors = previous if isinstance(previous, list) else [previous]
            reports.write_json(path, [*errors, message])
        except OSError:
            print(message, flush=True)
        if primary is None:
            raise
        primary.add_note(message)


@contextmanager
def removing_container(name: str, *, volume: str | None = None) -> Generator[None]:
    try:
        yield
    finally:
        subprocess.run(["docker", "rm", "--force", name], capture_output=True, check=False)
        if volume is not None:
            command("docker", "volume", "rm", volume)


def counters(observer: str) -> dict[str, reports.JSONValue]:
    # Read through the server's root, but run the reader in a separate cgroup
    root = "/proc/1/root/sys/fs/cgroup"
    pressure = "".join(
        f"sed 's/^/{resource}_pressure /' {root}/{resource}.pressure; "
        for resource in ("io", "memory", "cpu")
    )
    script = (
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
        "printf 'process_stat '; cat /proc/1/stat"
    )
    return reports.parse_counters(container_command(observer, "sh", "-c", script))


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
    script = (
        'exec 3<>/tmp/perf-ack 4<>/tmp/perf-control; printf "%s\\n" "$1" >&4; '
        'read -r -t 5 reply <&3 && test "$reply" = ack'
    )
    container_command(observer, "bash", "-c", script, "perf-control", action)


def healthy(port: int, *, expected: int = 200) -> bool:
    try:
        with closing(http.client.HTTPConnection("127.0.0.1", port, timeout=2)) as connection:
            connection.request("GET", "/health")
            response = connection.getresponse()
            response.read()
            return response.status == expected
    except OSError, http.client.HTTPException:
        return False


@dataclass(frozen=True)
class LoadGenerator:
    invocation: list[str]
    api_port: int
    version: str
    sha256: str
    container: str
    kind: GeneratorKind = "k6"

    def control(self, output: Path, *, resume: bool = False) -> dict[str, reports.JSONValue]:
        if self.kind == "k6":
            return k6_status(self.api_port, resume=resume)
        if resume:
            (output / "generator.start").touch()
            return {}
        return {"ready": (output / "generator.ready").is_file()}

    def sample(self) -> dict[str, reports.JSONValue]:
        values = container_command(
            self.container,
            "sh",
            "-c",
            "cat /sys/fs/cgroup/cpu.stat; printf 'memory '; cat /sys/fs/cgroup/memory.current",
        )
        rows = {line.split()[0]: int(line.split()[1]) for line in values.splitlines()}

        return {"client_cpu_s": rows["usage_usec"] / 1e6, "client_cgroup_bytes": rows["memory"]}


@contextmanager
def load_generator(
    config: RunConfig,
    container: str,
    output: Path,
    *,
    image: images.ImageIdentity,
    workspace: Path,
) -> Generator[LoadGenerator]:
    name = f"{container}-load"
    environment = reports.mapping(reports.read_json(output / "environment.json"))
    vm_memory = reports.integer(reports.mapping(environment["docker_info"]), "MemTotal")
    # Leave space for the API and Docker; contain generator failures in its own cgroup
    client_memory = max(vm_memory // 2, vm_memory - 4 * 2**30)
    native = config.generator == "cpp"
    reports.write_json(output / "generator-image.json", image)
    version = images.image_command(
        image,
        "/app/load_generator" if native else "/usr/bin/k6",
        "--version" if native else "version",
    )
    api_port = (
        0 if native else int(command("docker", "port", container, "6565/tcp").rsplit(":", 1)[1])
    )
    prefix = [
        "docker",
        "run",
        f"--name={name}",
        "--cap-drop=ALL",
        f"--memory={client_memory}",
        f"--memory-swap={client_memory}",
        f"--network=container:{container}",
        "--ulimit=nofile=1048576:1048576",
        f"--mount=type=bind,source={workspace},target=/work,readonly",
        f"--mount=type=bind,source={ROOT / 'seed'},target=/seed,readonly",
        f"--mount=type=bind,source={output},target=/results",
    ]
    settings = [
        "--env=BASE_URL=http://127.0.0.1:3000",
        f"--env=VUS={config.vus}",
        f"--env=DURATION={config.hold}",
        f"--env=RAMP_UP={config.ramp}",
        "--env=TOKENS=/seed/tokens.json",
    ]
    if native:
        invocation = [
            *prefix,
            *settings,
            "--entrypoint=/app/load_generator",
            image["image_id"],
            "--summary-export",
            "/results/k6-summary.json",
            "--local-ips=127.0.0.2-127.0.0.33",
            *(["--seed", str(config.seed)] if config.seed is not None else []),
            "--ready-file",
            "/results/generator.ready",
            "--start-gate",
            "/results/generator.start",
            *(["--diagnostics"] if config.client_diagnostics else []),
        ]
    else:
        invocation = [
            *prefix,
            f"--env=GOMEMLIMIT={client_memory * 9 // 10}B",
            image["image_id"],
            "run",
            "--quiet",
            "--paused",
            "--address=0.0.0.0:6565",
            "--local-ips=127.0.0.2-127.0.0.33",
            "--summary-export=/results/k6-summary.json",
            *settings,
            "/work/bench/load.js",
        ]
    try:
        yield LoadGenerator(
            invocation,
            api_port,
            version,
            image["binary_sha256"],
            name,
            config.generator,
        )
    finally:
        with preserving_failure(output, "generator cleanup"), removing_container(name):
            inspected = subprocess.run(["docker", "inspect", name], capture_output=True, text=True)
            (output / "load-generator.json").write_text(inspected.stdout, encoding="utf-8")


def load(config: RunConfig, generator: LoadGenerator, observer: str, output: Path) -> int:
    warmup, hold = config.timing
    profiling = config.perf != "off"
    ticks = int(container_command(observer, "getconf", "CLK_TCK"))
    samples: list[dict[str, reports.JSONValue]] = []
    with (output / "load.log").open("w", encoding="utf-8") as log:
        with subprocess.Popen(generator.invocation, stdout=log, stderr=subprocess.STDOUT) as child:
            try:
                initializing = time.monotonic()
                ready_deadline = initializing + 120
                next_init_sample = next_init_progress = initializing
                while child.poll() is None:
                    try:
                        status = generator.control(output)
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
                                    **generator.sample(),
                                }
                            )
                        except subprocess.CalledProcessError:
                            pass
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
                else:
                    raise RuntimeError("generator exited during initialization; inspect load.log")
                before = counters(observer)
                started = time.monotonic()
                generator.control(output, resume=True)
                resumed = time.monotonic() - started
                next_sample = next_progress = 0.0
                window: dict[str, reports.JSONValue] = {
                    "warmup_s": warmup,
                    "hold_s": hold,
                    "resume_ack_s": resumed,
                }
                profile_before: dict[str, reports.JSONValue] | None = None
                profile_after: dict[str, reports.JSONValue] | None = None
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
                    if profiling and profile_after is None and elapsed >= warmup + hold:
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
                        print(f"{config.label}: sampling complete; draining workload", flush=True)
                    if elapsed < next_sample:
                        time.sleep(0.1)
                        continue
                    next_sample = elapsed + 2
                    try:
                        client_started = time.monotonic() - started
                        client_sample = generator.sample()
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
                    except subprocess.CalledProcessError as error:
                        try:
                            child.wait(timeout=15)
                        except subprocess.TimeoutExpired:
                            raise error from None
                        break
                code = child.wait(timeout=15)
                if profiling and profile_after is None:
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
            "workload": config.workload,
            "exit_code": code,
            "wall_seconds": time.monotonic() - started,
            "cgroup_before": before,
            "cgroup_after": after,
            "cpu_scope": "server_cgroup",
            "clock_ticks_per_second": ticks,
            "initial_generator_status": status,
            "cpu": reports.cpu_delta(before, after, ticks),
            "load_generator": "docker_loopback",
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
    image: images.ImageIdentity,
    telemetry: reports.TelemetryMode = "off",
    workspace: Path | None = None,
) -> Generator[tuple[str, int]]:
    workspace = workspace or ROOT
    unique = f"td-bench-{uuid.uuid4().hex[:10]}"
    volume = unique + "-data"
    seed = (database or ROOT / "seed/feed.db").resolve()
    seeded = seed.is_file()
    if database is None and not seeded:
        raise ValueError("seed/feed.db is missing")
    launch = "exec /app/server"
    if seeded:
        launch = f"cp {shlex.quote('/seed/' + seed.name)} /data/feed.db && {launch}"
    command("docker", "volume", "create", volume)
    try:
        command(
            "docker",
            "run",
            "--rm",
            "--user=0",
            f"--volume={volume}:/data",
            "--entrypoint=chown",
            image["image_id"],
            "10001:10001",
            "/data",
        )
        command(
            "docker",
            "run",
            "--detach",
            f"--name={unique}",
            "--cpus=1",
            "--memory=2g",
            "--memory-swap=2g",
            "--cap-drop=ALL",
            f"--security-opt=seccomp={workspace / 'src/docker/seccomp.json'}",
            "--ulimit=nofile=65535:65535",
            f"--env=JWT_SECRET={secret}",
            *(["--env=TDS_TELEMETRY_PATH=/data/telemetry.jsonl"] if telemetry != "off" else []),
            "--publish=127.0.0.1::3000",
            "--publish=127.0.0.1::6565",
            f"--volume={volume}:/data",
            f"--mount=type=bind,source={seed.parent},target=/seed,readonly",
            "--entrypoint=sh",
            image["image_id"],
            "-c",
            launch,
        )
        port = int(command("docker", "port", unique, "3000/tcp").rsplit(":", 1)[1])
        for _ in range(120):
            if healthy(port, expected=200 if seeded else 503):
                break
            time.sleep(0.5)
        else:
            raise RuntimeError("container did not become healthy")
        version = reports.mapping(
            reports.decode_json(container_command(unique, "/app/server", "--version").encode())
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
        inspected = reports.decode_json(command("docker", "inspect", unique).encode())
        if not isinstance(inspected, list) or len(inspected) != 1:
            raise RuntimeError("expected one running server container")
        running_image = reports.text(reports.mapping(inspected[0]), "Image")
        if running_image != image["image_id"]:
            raise RuntimeError("running server image does not match the verified image")
        reports.write_json(
            output / "environment.json",
            {
                "host": platform.platform(),
                "logical_cpus": psutil.cpu_count(),
                "host_memory_bytes": psutil.virtual_memory().total,
                "docker_info": reports.decode_json(
                    command("docker", "info", "--format", "{{json .}}").encode()
                ),
                "container": inspected,
                "server_identity": image,
                "runtime": version,
                "telemetry": telemetry,
                "telemetry_mode_verified": actual_telemetry is not None,
                "filesystem": container_command(unique, "df", "-T", "/data"),
                "limits": container_command(
                    unique,
                    "sh",
                    "-c",
                    "ulimit -n; cat /sys/fs/cgroup/cpu.max /sys/fs/cgroup/memory.max /sys/fs/cgroup/memory.swap.max",
                ),
                "git_commit": command("git", "-C", str(ROOT), "rev-parse", "HEAD"),
                "git_status": command("git", "-C", str(ROOT), "status", "--short"),
                "workload_sha256": file_hash(workspace / "bench/load.js"),
                "seed_sha256": file_hash(seed) if seeded else None,
            },
        )
        yield unique, port
    finally:
        with (
            preserving_failure(output, "server cleanup"),
            removing_container(unique, volume=volume),
        ):
            try:
                subprocess.run(
                    ["docker", "stop", "--time=10", unique],
                    capture_output=True,
                    check=False,
                    timeout=20,
                )
            except subprocess.TimeoutExpired:
                print("Server stop timed out; removing the container.", flush=True)
            state = subprocess.run(["docker", "inspect", unique], capture_output=True, text=True)
            (output / "container-final.json").write_text(state.stdout, encoding="utf-8")
            logs = subprocess.run(["docker", "logs", unique], capture_output=True, text=True)
            (output / "server.log").write_text(logs.stdout + logs.stderr, encoding="utf-8")
            if telemetry != "off":
                collect_telemetry(unique, output)


def collect_telemetry(container: str, output: Path) -> None:
    copied = subprocess.run(
        ["docker", "cp", f"{container}:/data/telemetry.jsonl", str(output / "telemetry.jsonl")],
        capture_output=True,
        text=True,
        check=False,
        timeout=10,
    )
    if copied.returncode != 0:
        raise RuntimeError(copied.stderr.strip() or "server telemetry export is missing")


def collect_profile(
    container: str,
    output: Path,
    *,
    image: images.ImageIdentity,
    scope: reports.ProfileScope = "user",
) -> None:
    common = [
        "docker",
        "run",
        "--rm",
        "--network=none",
        f"--pid=container:{container}",
        "--cap-drop=ALL",
        *(["--user=0", "--cap-add=SYSLOG", "--cap-add=SYS_PTRACE"] if scope != "user" else []),
        f"--mount=type=bind,source={output},target=/profile,readonly",
        image["image_id"],
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
        output / "perf-analysis.json", reports.analyze_profile(stacks, statistics, scope=scope)
    )


@contextmanager
def observing(
    container: str,
    output: Path,
    *,
    profiling: bool,
    scope: reports.ProfileScope = "user",
    image: images.ImageIdentity,
    profiler_image: images.ImageIdentity | None = None,
) -> Generator[str]:
    name = f"{container}-observer"
    launch = ["--read-only", "--entrypoint=sleep", image["image_id"], "infinity"]
    if profiling:
        if profiler_image is None:
            raise ValueError("profiling requires a verified profiler image")
        if image["binary_sha256"] != profiler_image["binary_sha256"]:
            raise RuntimeError("profiler symbols do not match the running server")
        launch = [
            "--cap-add=PERFMON",
            *(["--cap-add=SYSLOG", "--cap-add=SYS_PTRACE"] if scope != "user" else []),
            "--entrypoint=sh",
            profiler_image["image_id"],
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
            if command("docker", "inspect", "--format={{.State.Running}}", name) != "true":
                raise RuntimeError(command("docker", "logs", name))
            perf_control(name, "ping")
            reports.write_json(
                output / "sampler.json",
                {
                    "perf_version": container_command(name, "perf", "--version"),
                    "binary_sha256": image["binary_sha256"],
                    "container": reports.decode_json(command("docker", "inspect", name).encode()),
                },
            )
        yield name
        completed = True
    finally:
        with preserving_failure(output, "profiler cleanup"), removing_container(name):
            if created and profiling:
                command("docker", "stop", "--signal=SIGINT", "--timeout=15", name)
                logs = subprocess.run(["docker", "logs", name], capture_output=True, text=True)
                (output / "perf.log").write_text(logs.stdout + logs.stderr, encoding="utf-8")
                exit_code = command("docker", "inspect", "--format={{.State.ExitCode}}", name)
                if exit_code not in ("0", "130"):
                    raise RuntimeError(f"perf exited with {exit_code}; see perf.log")
                command("docker", "cp", f"{name}:/tmp/perf.data", str(output / "perf.data"))
                if completed and profiler_image is not None:
                    collect_profile(container, output, scope=scope, image=profiler_image)


def run_workload(
    config: RunConfig,
    output: Path,
    *,
    image: images.ImageIdentity,
    source: sources.SourceSnapshot,
    profiler_image: images.ImageIdentity | None = None,
) -> int:
    generator_image = (
        images.ensure_generator(source)
        if config.generator == "cpp"
        else images.inspect(images.K6_IMAGE, "/usr/bin/k6")
    )
    with (
        server(
            output,
            image=image,
            telemetry=config.telemetry,
            workspace=source.root,
        ) as (container, _port),
        observing(
            container,
            output,
            profiling=config.perf != "off",
            scope=config.perf if config.perf != "off" else "user",
            image=image,
            profiler_image=profiler_image,
        ) as observer,
        load_generator(
            config, container, output, image=generator_image, workspace=source.root
        ) as generator,
    ):
        return load(config, generator, observer, output)


def _record_image(
    run: runs.RunRecord,
    component: str,
    identity: images.ImageIdentity | dict[str, reports.JSONValue],
) -> None:
    run.record_build(component, cast("dict[str, reports.JSONValue]", identity))
    run.record_binary(
        component, {"image_id": identity["image_id"], "sha256": identity["binary_sha256"]}
    )


def _measure(
    config: RunConfig,
    run: runs.RunRecord,
    source: sources.SourceSnapshot,
    *,
    image: str | None,
    reuse_server: bool,
) -> int:
    if source.base_commit != run.manifest["anchor"]:
        raise RuntimeError("HEAD changed while capturing the run's build inputs")
    source_identity: dict[str, reports.JSONValue] = {
        "base_commit": source.base_commit,
        "patch_sha256": source.patch_sha256,
        "source_sha256": source.source_sha256,
    }
    (run.raw / "observer.patch").write_bytes(source.patch)
    run.record_observer({**source_identity, "patch": "raw/observer.patch"})
    rebuilt = not reuse_server and image is None
    identity = (
        images.build(source, telemetry=config.telemetry)
        if rebuilt
        else images.inspect(image or images.image_name(telemetry=config.telemetry), "/app/server")
    )
    _record_image(run, "server", identity)
    if identity["source"] == source_identity:
        run.record_source(source.base_commit, source.patch, source.source_sha256)
    elif rebuilt:
        raise RuntimeError("built server image source does not match the frozen build inputs")
    profiler_image = None
    if config.perf != "off":
        profiler_image = images.build(source, "profiler", telemetry=config.telemetry)
        _record_image(run, "profiler", profiler_image)
    if config.telemetry != "off":
        print(f"Server telemetry: {config.telemetry}; collection overhead is included.", flush=True)
    return run_workload(
        config,
        run.raw,
        image=identity,
        source=source,
        profiler_image=profiler_image,
    )


def _error_code(error: BaseException) -> int:
    if isinstance(error, KeyboardInterrupt):
        return 130
    return error.returncode if isinstance(error, subprocess.CalledProcessError) else 1


def _error_text(error: BaseException) -> str:
    return "interrupted" if isinstance(error, KeyboardInterrupt) else str(error)


def execute_workload(
    config: RunConfig, *, image: str | None = None, reuse_server: bool = False
) -> int:
    _ = config.timing
    anchor = command("git", "-C", str(ROOT), "rev-parse", "HEAD")
    run = runs.create_run(
        ROOT,
        anchor,
        config.label,
        config.workload,
        {
            "server_telemetry": config.telemetry,
            "client_diagnostics": config.client_diagnostics,
            "perf": config.perf,
        },
        {"base_commit": anchor},
    )
    primary: BaseException | None = None
    code = 1
    secondary: list[str] = []
    try:
        with sources.capture(ROOT) as source:
            try:
                code = _measure(config, run, source, image=image, reuse_server=reuse_server)
            except (Exception, KeyboardInterrupt) as error:
                primary = error
                code = _error_code(error)
    except (Exception, KeyboardInterrupt) as error:
        if primary is None:
            primary = error
            code = _error_code(error)
        else:
            secondary.append(_error_text(error))
    if primary is not None:
        secondary.extend(getattr(primary, "__notes__", []))
    summary: dict[str, reports.JSONValue] = {}
    try:
        summary = reports.run_summary(run.raw, label=config.label, telemetry=config.telemetry)
        errors = summary.get("collection_errors", [])
        if not isinstance(errors, list) or any(not isinstance(item, str) for item in errors):
            raise ValueError("collection errors must be an array of strings")
        secondary.extend(item for item in errors if isinstance(item, str))
        measured_run = run.raw / "run.json"
        if (
            primary is not None
            and not isinstance(primary, KeyboardInterrupt)
            and measured_run.is_file()
        ):
            measured_code = reports.integer(
                reports.mapping(reports.read_json(measured_run)), "exit_code"
            )
            if measured_code != 0:
                code = measured_code
                secondary.append(_error_text(primary))
                primary = None
        generator_identity = run.raw / "generator-image.json"
        if generator_identity.is_file():
            _record_image(run, "generator", reports.mapping(reports.read_json(generator_identity)))
    except Exception as error:
        secondary.append(_error_text(error))
    secondary = list(dict.fromkeys(secondary))
    if code == 0 and secondary:
        code = 1
    status: runs.TerminalStatus = (
        "passed" if code == 0 else "interrupted" if code == 130 else "failed"
    )
    message = None
    if primary is not None:
        message = _error_text(primary)
    elif code not in (0, 1):
        message = f"load generator exited {code}"
    code = run.finish(status, summary, code, message, secondary)
    reports.show_saved_report(reports.read_json(run.path / "summary.json"))
    for failure in ([message] if message is not None else []) + secondary:
        print(failure, file=sys.stderr)
    print(f"Run: {run.path}", flush=True)
    return code
