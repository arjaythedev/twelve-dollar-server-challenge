#!/usr/bin/env python3
"""Warm up a running feed server, search its k6 load limit, and write reports.

The caller deploys the app (directly, in Docker, or in Kubernetes) and
passes its URL. This tool never starts, resets, or stops the app.
"""

import argparse
import datetime
import hashlib
import json
import math
import os
import platform
import resource
import shlex
import signal
import subprocess
import sys
import threading
import time
import traceback
import urllib.request
import uuid
from pathlib import Path

from benchmark_report import report

HERE = Path(__file__).resolve().parent
WORKLOAD = HERE.parent.parent / "bench" / "load.js"
K6_IMAGE = "grafana/k6:2.2.0@sha256:9bd01d6941fca969cb61bb57d2da5ee9b385fe2aa8881df3798c196564d6ace6"
# --k6 exec contract: the k6 container holds load.js and tokens.json here, and
# a writable results/ directory.
REMOTE = Path("/work")
CHECKS = ("feed 200", "post 200", "like 200/201", "create 201")
THRESHOLDS = (
    ("http_req_duration", "p(95)<500", "p(95)", 500),
    ("http_req_duration", "p(99)<1000", "p(99)", 1000),
    ("http_req_failed", "rate<0.01", "value", 0.01),
)
RAMP_DOWN_SECONDS = 30
# Covers k6's graceful stop plus process or container startup.
RUN_ALLOWANCE_SECONDS = 60
SAMPLE_SECONDS = 2
# k6 closes its connections when a run ends, and Linux keeps each closed
# connection's local port in TIME_WAIT for 60 seconds. A persistent k6 container
# that starts the next run sooner can run out of ports and fail to connect.
TIME_WAIT_SCRIPT = (
    "cat /proc/net/tcp /proc/net/tcp6 2>/dev/null | awk '$4 == \"06\"' | wc -l"
)
TIME_WAIT_LIMIT = 100
TIME_WAIT_TIMEOUT_SECONDS = 120


class ExecutionError(Exception):
    pass


class AppStopped(ExecutionError):
    pass


class BudgetExpired(Exception):
    pass


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def number(value):
    if (
        isinstance(value, bool)
        or not isinstance(value, (int, float))
        or not math.isfinite(value)
        or value < 0
    ):
        raise ExecutionError("Invalid numeric value in k6 summary")
    return value


def parse_summary(summary, exit_code, expected_vus=None, minimum_seconds=None):
    """Validate k6's pinned legacy JSON contract, not its human-readable output."""
    if exit_code not in (0, 99):
        raise ExecutionError(f"k6 execution failed with exit code {exit_code}")
    try:
        metrics = summary["metrics"]
        requests = number(metrics["http_reqs"]["count"])
        if requests == 0:
            raise ExecutionError("k6 issued no requests")
        if expected_vus is not None and (
            number(metrics["vus_max"]["value"]) != expected_vus
            or number(metrics["vus"]["max"]) != expected_vus
        ):
            raise ExecutionError("k6 did not reach the requested virtual-user count")
        rate = number(metrics["http_reqs"]["rate"])
        if rate == 0 or (
            minimum_seconds is not None and requests / rate < minimum_seconds
        ):
            raise ExecutionError("k6 did not complete the requested stage duration")
        thresholds = {}
        for metric, expression, key, limit in THRESHOLDS:
            value = number(metrics[metric][key])
            failed = metrics[metric]["thresholds"][expression]
            # In --summary-export, the boolean is 'failed', not 'ok'.
            if type(failed) is not bool or failed != (value >= limit):
                raise ExecutionError(f"Inconsistent threshold result: {expression}")
            thresholds[expression] = not failed
        passed = all(thresholds.values())
        if (exit_code == 0) != passed:
            raise ExecutionError("k6 exit code disagrees with threshold outcomes")
        groups = summary["root_group"]["checks"]
        checks = {}
        for name in CHECKS:
            check = groups.get(name, {})
            checks[name] = {
                "passed": number(check.get("passes", 0)),
                "failed": number(check.get("fails", 0)),
            }
        if checks["feed 200"]["passed"] + checks["feed 200"]["failed"] == 0:
            raise ExecutionError("The expected feed workload was not exercised")
        return {
            "status": "passed" if passed else "threshold failed",
            "thresholds": thresholds,
            "checks": checks,
            "metrics": {
                "requests": requests,
                "rps": rate,
                "avg_ms": number(metrics["http_req_duration"]["avg"]),
                "p95_ms": number(metrics["http_req_duration"]["p(95)"]),
                "p99_ms": number(metrics["http_req_duration"]["p(99)"]),
                "max_ms": number(metrics["http_req_duration"]["max"]),
                "error_percent": number(metrics["http_req_failed"]["value"]) * 100,
            },
        }
    except (KeyError, TypeError, AttributeError) as error:
        raise ExecutionError(f"Missing or invalid k6 summary field: {error}") from error


def validate_events(path, exit_code):
    """Iteration exceptions can be logged even when k6 exits successfully."""
    if not path.is_file():
        raise ExecutionError("k6 did not produce its structured event log")
    for line in path.read_text(encoding="utf-8").splitlines():
        event = json.loads(line)
        message = event.get("msg", "")
        threshold_error = (
            exit_code == 99
            and message.startswith("thresholds on metrics ")
            and message.endswith(" have been crossed")
        )
        if event.get("level") == "error" and not threshold_error:
            raise ExecutionError(f"k6 logged an execution error: {message}")


def search(run, start, maximum, step, confirmations):
    """Only genuine threshold failures shrink the passing/failing interval."""
    low, high = 0, None
    candidate = start
    while True:
        if run("search", candidate):
            low = candidate
            if candidate == maximum:
                break
            candidate = min(candidate * 2, maximum)
        else:
            high = candidate
            break
    if high is not None:
        while high - low > step:
            candidate = ((low + high) // (2 * step)) * step
            if run("refine", candidate):
                low = candidate
            else:
                high = candidate
    candidate = low
    while candidate >= step:
        if all(run("confirm", candidate) for _ in range(confirmations)):
            return candidate, high is None and candidate == maximum
        candidate -= step
    return None, False


def execute(args, *, log=None, timeout=60, env=None):
    """Return stdout, or only the exit code when output goes to a log file."""
    if log is None:
        result = subprocess.run(
            args,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
            env=env,
        )
        if result.returncode:
            raise ExecutionError(
                f"{shlex.join(args)} exited {result.returncode}: {result.stderr.strip()}"
            )
        return result.stdout
    with open(log, "w", encoding="utf-8") as stream:
        return subprocess.run(
            args,
            stdin=subprocess.DEVNULL,
            stdout=stream,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
            env=env,
            start_new_session=True,
        ).returncode


def healthy(url):
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(f"{url}/health", timeout=5) as response:
            data = json.load(response)
            return (
                response.status == 200
                and data.get("status") == "ok"
                and data.get("db") == "ok"
            )
    except (OSError, ValueError):
        return False


def wait_healthy(url, seconds):
    deadline = time.monotonic() + seconds
    while not healthy(url):
        if time.monotonic() > deadline:
            raise ExecutionError(f"{url}/health was not healthy within {seconds}s")
        time.sleep(1)


def wait_for_ports(remote):
    """Wait until the previous run's closed k6 connections release their ports."""
    deadline = time.monotonic() + TIME_WAIT_TIMEOUT_SECONDS
    announced = False
    while (
        count := int(execute([*remote, "sh", "-c", TIME_WAIT_SCRIPT]))
    ) > TIME_WAIT_LIMIT:
        if time.monotonic() > deadline:
            raise ExecutionError(f"k6 still holds {count} ports in TIME_WAIT")
        if not announced:
            print(
                f"  Waiting for {count} closed k6 connections to release their ports",
                flush=True,
            )
            announced = True
        time.sleep(5)


def cpu_seconds(text):
    """Parse ps TIME values such as 1-02:03:04, 01:02:03, or 12:34.56."""
    days, _, clock = text.rpartition("-")
    seconds = 0.0
    for part in clock.split(":"):
        seconds = seconds * 60 + float(part)
    return seconds + int(days or 0) * 86400


class ProcessProbe:
    """Direct mode: one host process, read from /proc on Linux or ps on macOS."""

    source = "app process CPU time and resident memory"

    def __init__(self, pid):
        self.pid = pid

    def sample(self):
        try:
            os.kill(self.pid, 0)
        except ProcessLookupError as error:
            raise AppStopped(f"App process {self.pid} exited") from error
        except PermissionError:
            pass
        proc = Path(f"/proc/{self.pid}")
        if proc.is_dir():
            fields = (proc / "stat").read_text().rpartition(")")[2].split()
            cpu = (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
            status = (proc / "status").read_text().splitlines()
            rss_kib = next(int(l.split()[1]) for l in status if l.startswith("VmRSS:"))
        else:
            used, rss = execute(["ps", "-o", "time=,rss=", "-p", str(self.pid)]).split()
            cpu, rss_kib = cpu_seconds(used), int(rss)
        return {"cpu_seconds": cpu, "memory_mib": rss_kib / 1024, "oom_kills": None}


class CgroupProbe:
    """Container modes: cgroup v2 counters read through exec command prefixes.

    Several prefixes, such as the containers of one pod, are summed.
    """

    source = "container cgroup CPU, memory.current including page cache, and OOM kills"
    SCRIPT = (
        "cd /sys/fs/cgroup && grep '^usage_usec ' cpu.stat"
        " && echo memory_current $(cat memory.current)"
        " && grep '^oom_kill ' memory.events"
    )

    def __init__(self, prefixes):
        self.prefixes = [shlex.split(prefix) for prefix in prefixes]
        self.cpu = None

    def limits(self):
        """Record the enforced limits, e.g. the challenge VPS budget."""
        script = "cd /sys/fs/cgroup && cat cpu.max memory.max && (cat memory.swap.max || echo n/a)"
        cpu, memory, swap = execute(
            [*self.prefixes[0], "sh", "-c", script]
        ).splitlines()
        quota, period = cpu.split()

        def mib(value):
            return None if value in ("max", "n/a") else int(value) / 1024**2

        return {
            "cpus": None if quota == "max" else int(quota) / int(period),
            "memory_mib": mib(memory),
            "swap_mib": mib(swap),
        }

    def sample(self):
        parts = [
            dict(
                line.split()
                for line in execute([*prefix, "sh", "-c", self.SCRIPT]).splitlines()
            )
            for prefix in self.prefixes
        ]
        cpu = [int(part["usage_usec"]) / 1e6 for part in parts]
        if self.cpu and any(now < before for now, before in zip(cpu, self.cpu)):
            raise AppStopped("A container restarted")
        self.cpu = cpu
        return {
            "cpu_seconds": sum(cpu),
            "memory_mib": sum(int(part["memory_current"]) for part in parts) / 1024**2,
            "oom_kills": sum(int(part["oom_kill"]) for part in parts),
        }


class Sampler:
    """Samples the app in the background while k6 runs."""

    def __init__(self, probe):
        self.probe = probe
        self.samples = []
        self.error = None
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def _loop(self):
        while not self.error:
            self._take()
            if self.stopped.wait(SAMPLE_SECONDS):
                return

    def _take(self):
        try:
            self.samples.append((time.monotonic(), self.probe.sample()))
        except (
            ExecutionError,
            OSError,
            ValueError,
            KeyError,
            IndexError,
            StopIteration,
            subprocess.SubprocessError,
        ) as error:
            self.error = error

    def finish(self):
        self.stopped.set()
        self.thread.join()
        if not self.error:
            self._take()
        if isinstance(self.error, AppStopped):
            raise self.error
        if self.error or len(self.samples) < 2:
            reason = str(self.error or "fewer than two samples")
            return {"source": self.probe.source, "unavailable": reason}
        (start, first), (end, last) = self.samples[0], self.samples[-1]
        if last["cpu_seconds"] < first["cpu_seconds"]:
            raise ExecutionError("The app restarted during the run")
        oom_kills = (
            None
            if last["oom_kills"] is None
            else last["oom_kills"] - first["oom_kills"]
        )
        if oom_kills:
            raise ExecutionError(f"The app was OOM-killed {oom_kills} time(s)")
        return {
            "source": self.probe.source,
            "samples": len(self.samples),
            "cpu_percent": (last["cpu_seconds"] - first["cpu_seconds"])
            / (end - start)
            * 100,
            "peak_memory_mib": max(sample["memory_mib"] for _, sample in self.samples),
            "oom_kills": oom_kills,
        }


class FlightRecorder:
    """Records each run with Java Flight Recorder through jcmd in the app container."""

    REMOTE_DIRECTORY = "/tmp"

    def __init__(self, prefix):
        self.prefix = shlex.split(prefix)
        self.pid = None

    def start(self, name):
        if self.pid is None:
            # jcmd -l lists "<pid> <main class or JAR>", including jcmd itself.
            listing = execute([*self.prefix, "jcmd", "-l"])
            pids = [
                line.split()[0]
                for line in listing.splitlines()
                if line.split()[1:2] and line.split()[1].endswith("server.jar")
            ]
            if len(pids) != 1:
                raise ExecutionError(
                    f"Expected one server.jar JVM, jcmd -l listed: {listing}"
                )
            self.pid = pids[0]
        self._jcmd(
            "JFR.start",
            f"name={name}",
            "settings=profile",
            f"filename={self.REMOTE_DIRECTORY}/{name}.jfr",
        )

    def stop(self, name, destination):
        remote = f"{self.REMOTE_DIRECTORY}/{name}.jfr"
        self._jcmd("JFR.stop", f"name={name}")
        destination.parent.mkdir(exist_ok=True)
        with open(destination, "wb") as stream:
            result = subprocess.run(
                [*self.prefix, "cat", remote],
                stdin=subprocess.DEVNULL,
                stdout=stream,
                stderr=subprocess.PIPE,
                timeout=600,
                check=False,
            )
        if result.returncode:
            raise ExecutionError(
                f"Cannot copy {remote}: {result.stderr.decode().strip()}"
            )
        execute([*self.prefix, "rm", "-f", remote])

    def _jcmd(self, *command):
        execute([*self.prefix, "jcmd", self.pid, *command])


def k6_command(args, output, container, vus, hold, stem):
    variables = {
        "BASE_URL": args.k6_url or args.url,
        "VUS": vus,
        "DURATION": f"{hold}s",
        "RAMP_UP": f"{args.ramp_seconds}s",
    }
    if args.k6 == "local":
        prefix = [args.k6_bin]
        results, workload = output / "k6", WORKLOAD
        variables["TOKENS"] = args.tokens.resolve()
    elif args.k6 == "exec":
        prefix = shlex.split(args.k6_exec) + ["k6"]
        results, workload = REMOTE / "results", REMOTE / "load.js"
        variables["TOKENS"] = REMOTE / "tokens.json"
    else:
        prefix = ["docker", "run", "--rm", "--name", container]
        prefix += ["--user", f"{os.getuid()}:{os.getgid()}", "-e", "NO_PROXY=*"]
        prefix += ["-v", f"{WORKLOAD.parent}:/bench:ro"]
        prefix += ["-v", f"{args.tokens.resolve()}:/tokens.json:ro"]
        prefix += ["-v", f"{output / 'k6'}:/results"]
        if args.docker_network:
            prefix += ["--network", args.docker_network]
        prefix.append(K6_IMAGE)
        results, workload = Path("/results"), Path("/bench/load.js")
        variables["TOKENS"] = "/tokens.json"
    return [
        *prefix,
        "run",
        "--quiet",
        "--no-usage-report",
        "--new-machine-readable-summary=false",
        "--log-format=json",
        f"--log-output=file={results / (stem + '.events.jsonl')}",
        f"--summary-export={results / (stem + '.json')}",
        *(f"--env={key}={value}" for key, value in variables.items()),
        str(workload),
    ]


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="Base URL checked by this tool")
    parser.add_argument(
        "--tokens", type=Path, help="Seed tokens.json; not used with --k6 exec"
    )
    parser.add_argument("--mode", default="external", help="Deployment label")
    parser.add_argument("--k6", choices=("local", "docker", "exec"), default="local")
    parser.add_argument("--k6-bin", default="k6", help="Local k6 executable")
    parser.add_argument(
        "--k6-exec",
        help="With --k6 exec, the prefix that runs commands in a k6 container, "
        'e.g. "kubectl exec -n NS k6 --". The container must hold /work/load.js, '
        "/work/tokens.json, and a writable /work/results.",
    )
    parser.add_argument("--k6-url", help="Base URL seen by k6; defaults to --url")
    parser.add_argument("--docker-network", help="Network for the k6 container")
    probe = parser.add_mutually_exclusive_group()
    probe.add_argument("--app-pid", type=int, help="Sample this host process")
    probe.add_argument(
        "--app-exec",
        action="append",
        help='Sample cgroup counters through a command prefix, e.g. "docker exec ID"; '
        "repeat it to sum several containers",
    )
    parser.add_argument(
        "--app-limits",
        type=json.loads,
        help="Enforced limits as JSON, when the probe cannot read them",
    )
    parser.add_argument(
        "--jfr-exec",
        help="Record each run with Java Flight Recorder through this command prefix, "
        'e.g. "kubectl exec -n NS server -c app --". The container needs jcmd, cat, '
        "and rm on its PATH and a writable /tmp.",
    )
    parser.add_argument("--output", type=Path, help="New report directory")
    parser.add_argument("--wait-seconds", type=int, default=120)
    parser.add_argument("--warmup-vus", type=int, default=1000)
    parser.add_argument("--warmup-seconds", type=int, default=120)
    parser.add_argument("--start-vus", type=int, default=2500)
    parser.add_argument("--max-vus", type=int, default=20000)
    parser.add_argument("--step", type=int, default=250)
    parser.add_argument("--probe-seconds", type=int, default=60)
    parser.add_argument("--confirm-seconds", type=int, default=300)
    parser.add_argument("--confirm-runs", type=int, default=1)
    parser.add_argument("--ramp-seconds", type=int, default=60)
    parser.add_argument(
        "--budget-seconds",
        type=int,
        default=3600,
        help="Load-search budget after the app is healthy",
    )
    args = parser.parse_args(argv)
    for key, value in vars(args).items():
        if isinstance(value, int) and value <= 0:
            parser.error(f"--{key.replace('_', '-')} must be positive")
    if (
        args.start_vus > args.max_vus
        or args.start_vus % args.step
        or args.max_vus % args.step
    ):
        parser.error("start/max users must be step multiples, with start <= max")
    if args.k6 != "exec" and not (args.tokens and args.tokens.is_file()):
        parser.error(f"--tokens {args.tokens} does not exist")
    if (args.k6 == "exec") != bool(args.k6_exec):
        parser.error("--k6-exec is required with, and only valid for, --k6 exec")
    return args


def main(argv=None):
    args = arguments(argv)
    suffix = uuid.uuid4().hex[:10]
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = (
        args.output or HERE / "benchmark-results" / f"{stamp}-{args.mode}-{suffix}"
    ).resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
        (output / "k6").mkdir()
    except OSError as error:
        print(f"Cannot create a new output directory: {error}", file=sys.stderr)
        return 2
    container = f"twelve-dollar-k6-{suffix}"
    probe = (
        ProcessProbe(args.app_pid)
        if args.app_pid
        else (CgroupProbe(args.app_exec) if args.app_exec else None)
    )
    recorder = FlightRecorder(args.jfr_exec) if args.jfr_exec else None
    workload_sha = sha256(WORKLOAD)
    state = {
        "status": "starting",
        "runs": [],
        "confirmed_vus": None,
        "environment": {
            "mode": args.mode,
            "url": args.url,
            "k6_url": args.k6_url or args.url,
            "k6": {"docker": K6_IMAGE, "exec": args.k6_exec}.get(args.k6, args.k6_bin),
            "app_probe": probe.source if probe else None,
            "host": platform.platform(),
            "host_logical_cpus": os.cpu_count(),
            "workload_sha256": workload_sha,
            "settings": {
                key: str(value) if isinstance(value, Path) else value
                for key, value in vars(args).items()
            },
        },
    }
    report(output, state)
    k6_env = {**os.environ, "NO_PROXY": "*", "no_proxy": "*"}
    exit_code = 0

    def interrupted(_signum, _frame):
        raise KeyboardInterrupt

    previous_handlers = {
        sig: signal.signal(sig, interrupted)
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
    }
    try:
        if args.k6 == "local":
            state["environment"]["k6"] = execute([args.k6_bin, "version"]).strip()
            # k6 holds about one connection per user; too few descriptors would look like a capacity limit.
            files = resource.getrlimit(resource.RLIMIT_NOFILE)[0]
            needed = max(args.warmup_vus, args.max_vus) + 1024
            if files != resource.RLIM_INFINITY and files < needed:
                raise ExecutionError(
                    f"Open-file limit {files} is below {needed}; raise it or lower the user cap"
                )
        elif args.k6 == "exec":
            remote = shlex.split(args.k6_exec)
            # The container brings its own copy of the workload; it must be this one.
            remote_sha = execute(
                [*remote, "sha256sum", str(REMOTE / "load.js")]
            ).split()
            if remote_sha[:1] != [workload_sha]:
                raise ExecutionError(
                    "The k6 container's load.js differs from bench/load.js"
                )
            state["environment"]["k6"] = execute([*remote, "k6", "version"]).strip()
        else:
            try:
                execute(["docker", "image", "inspect", K6_IMAGE])
            except ExecutionError:
                execute(["docker", "pull", "--quiet", K6_IMAGE], timeout=600)
        print(f"Waiting for {args.url}/health", flush=True)
        wait_healthy(args.url, args.wait_seconds)
        if args.app_limits is not None:
            state["environment"]["app_limits"] = args.app_limits
        elif isinstance(probe, CgroupProbe):
            state["environment"]["app_limits"] = probe.limits()
        deadline = time.monotonic() + args.budget_seconds
        state["status"] = "running"

        def run(phase, vus):
            hold = {"warmup": args.warmup_seconds, "confirm": args.confirm_seconds}.get(
                phase, args.probe_seconds
            )
            if args.k6 == "exec":
                wait_for_ports(remote)
            needed = (
                args.ramp_seconds + hold + RAMP_DOWN_SECONDS + RUN_ALLOWANCE_SECONDS
            )
            if deadline - time.monotonic() < needed:
                raise BudgetExpired("The time budget cannot fit the next complete run")
            identifier = len(state["runs"]) + 1
            stem = f"run-{identifier:03d}"
            item = {
                "id": identifier,
                "phase": phase,
                "vus": vus,
                "hold_seconds": hold,
                "started_at": now(),
                "status": "running",
                "log": f"{stem}.log",
            }
            state["runs"].append(item)
            report(output, state)
            print(f"Run {identifier}: {phase}, {vus} users, {hold}s hold", flush=True)
            started = time.monotonic()
            sampler = Sampler(probe) if probe else None
            recording = None
            profile = output / "jfr" / f"{stem}.jfr"
            try:
                if recorder:
                    recorder.start(stem)
                    recording = stem
                if sha256(WORKLOAD) != workload_sha:
                    raise ExecutionError("The workload changed during the search")
                if args.k6 == "exec":
                    # A k6 whose kubectl exec stream dropped may still be sending load.
                    execute([*remote, "sh", "-c", "pkill -x k6; true"])
                code = execute(
                    k6_command(args, output, container, vus, hold, stem),
                    log=output / item["log"],
                    timeout=max(1, deadline - time.monotonic()),
                    # Exec mode runs kubectl, which may need a proxy; k6 then runs remotely.
                    env=None if args.k6 == "exec" else k6_env,
                )
                item["exit_code"] = code
                if code in (137, -9):
                    raise ExecutionError(
                        "k6 was killed (exit 137), usually because it ran out of memory. "
                        "Give k6 more memory (in Docker mode, BENCH_K6=local) or lower BENCH_MAX_VUS."
                    )
                if args.k6 == "exec":
                    for name in (f"{stem}.json", f"{stem}.events.jsonl"):
                        path = REMOTE / "results" / name
                        try:
                            text = execute([*remote, "cat", str(path)], timeout=600)
                        except ExecutionError:
                            continue  # The checks below report missing output.
                        (output / "k6" / name).write_text(text, encoding="utf-8")
                validate_events(output / "k6" / f"{stem}.events.jsonl", code)
                item["events"] = f"k6/{stem}.events.jsonl"
                summary = output / "k6" / f"{stem}.json"
                if not summary.is_file():
                    raise ExecutionError(f"k6 exited {code} without a summary")
                item["summary"] = f"k6/{stem}.json"
                item.update(
                    parse_summary(
                        json.loads(summary.read_bytes()),
                        code,
                        expected_vus=vus,
                        minimum_seconds=args.ramp_seconds + hold + RAMP_DOWN_SECONDS,
                    )
                )
                item["resources"] = (
                    sampler.finish()
                    if sampler
                    else {"unavailable": "No app probe configured"}
                )
                if not healthy(args.url):
                    raise ExecutionError("The app is unhealthy after the run")
                if recording:
                    recording = None
                    recorder.stop(stem, profile)
                    item["profile"] = f"jfr/{stem}.jfr"
            except KeyboardInterrupt:
                item.update(status="interrupted", error="Interrupted during this run")
                raise
            except subprocess.TimeoutExpired as error:
                item.update(
                    status="budget exhausted",
                    error="The run exceeded the remaining search budget",
                )
                raise BudgetExpired(item["error"]) from error
            except Exception as error:
                message = (
                    str(error)
                    if isinstance(error, ExecutionError)
                    else f"{type(error).__name__}: {error}"
                )
                item.update(status="execution error", error=message)
                raise ExecutionError(message) from error
            finally:
                if sampler:
                    sampler.stopped.set()
                if recording:
                    # A failed run keeps its recording when the app still answers jcmd.
                    try:
                        recorder.stop(stem, profile)
                        item["profile"] = f"jfr/{stem}.jfr"
                    except (ExecutionError, OSError, subprocess.SubprocessError):
                        pass
                item["wall_seconds"] = time.monotonic() - started
                report(output, state)
            metrics = item["metrics"]
            print(
                f"  {item['status']}: p95={metrics['p95_ms']:.2f}ms, "
                f"p99={metrics['p99_ms']:.2f}ms, errors={metrics['error_percent']:.3f}%",
                flush=True,
            )
            return item["status"] == "passed"

        run("warmup", args.warmup_vus)
        confirmed, capped = search(
            run, args.start_vus, args.max_vus, args.step, args.confirm_runs
        )
        state["confirmed_vus"] = confirmed
        if capped:
            state["status"] = "capped"
            state["message"] = (
                "User cap reached without a failing probe; the result is a lower bound."
            )
        elif confirmed:
            state.update(
                status="completed", message="Search and confirmation finished."
            )
        else:
            state.update(
                status="no passing candidate",
                message="No candidate passed confirmation.",
            )
            exit_code = 1
        if args.confirm_seconds < 300:
            state["message"] += (
                " Confirmation was shorter than 5 minutes; this is a smoke test."
            )
        if args.ramp_seconds != 60:
            state["message"] += (
                " Custom ramp duration; not comparable to the default workload."
            )
    except BudgetExpired as error:
        state.update(status="budget exhausted", message=str(error))
        exit_code = 2
    except KeyboardInterrupt:
        state.update(
            status="interrupted", message="Interrupted. Completed runs are retained."
        )
        exit_code = 130
    except Exception as error:  # noqa: BLE001 - every failure must still produce a report
        if not isinstance(error, ExecutionError):
            (output / "traceback.txt").write_text(
                traceback.format_exc(), encoding="utf-8"
            )
        state.update(status="execution error", message=str(error))
        exit_code = 2
    finally:
        for sig in previous_handlers:
            signal.signal(sig, signal.SIG_IGN)
        if args.k6 == "docker":
            subprocess.run(
                ["docker", "rm", "--force", container], capture_output=True, check=False
            )
        report(output, state)
        for sig, handler in previous_handlers.items():
            signal.signal(sig, handler)
    print(
        f"Result: {state['status']}. Reports: {output / 'report.md'} and {output / 'report.html'}",
        flush=True,
    )
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
