#!/usr/bin/env python3
"""Run a bounded, isolated local k6 limit search and retain its evidence."""

import argparse
import datetime
import hashlib
import json
import logging
import math
import os
import platform
import signal
import subprocess
import sys
import time
import traceback
import urllib.request
import uuid
from pathlib import Path

from benchmark_report import report

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
LOGGER = logging.getLogger(__name__)
K6_IMAGE = "grafana/k6:2.2.0@sha256:9bd01d6941fca969cb61bb57d2da5ee9b385fe2aa8881df3798c196564d6ace6"
CHECKS = ("feed 200", "post 200", "like 200/201", "create 201")
THRESHOLDS = (
    ("http_req_duration", "p(95)<500", "p(95)", 500),
    ("http_req_duration", "p(99)<1000", "p(99)", 1000),
    ("http_req_failed", "rate<0.01", "value", 0.01),
)


class ExecutionError(Exception):
    pass


RAMP_DOWN_SECONDS = 30
GRACEFUL_STOP_SECONDS = 30
STARTUP_ALLOWANCE_SECONDS = 30


class BudgetExpired(Exception):
    pass


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")


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


def command(args, *, log=None, timeout=180, check=True):
    if log is None:
        result = subprocess.run(
            args,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            check=False,
            start_new_session=True,
        )
    else:
        with Path(log).open("w", encoding="utf-8") as stream:
            result = subprocess.run(
                args,
                stdout=stream,
                stderr=subprocess.STDOUT,
                timeout=timeout,
                check=False,
                start_new_session=True,
            )
    if check and result.returncode:
        detail = (
            result.stderr.strip()
            if log is None
            else f"see {log}\n"
            + "\n".join(Path(log).read_text(encoding="utf-8").splitlines()[-8:])
        )
        raise ExecutionError(
            f"Command exited {result.returncode}: {' '.join(map(str, args))}; {detail}"
        )
    return result.stdout if log is None else result.returncode


def app_resources(compose, container):
    files = command(
        compose
        + [
            "exec",
            "-T",
            "app",
            "sh",
            "-c",
            'set -e; cd /sys/fs/cgroup; cat cpu.stat memory.events; for f in memory.peak memory.swap.peak; do if [ -r "$f" ]; then printf \'%s %s\\n\' "$f" "$(cat "$f")"; else printf \'%s n/a\\n\' "$f"; fi; done',
        ]
    )
    try:
        inspected = json.loads(command(["docker", "inspect", container]))[0]
        values = dict(line.split() for line in files.splitlines())
        return {
            "cpu_usec": int(values["usage_usec"]),
            "peak_memory_mib": None
            if values["memory.peak"] == "n/a"
            else int(values["memory.peak"]) / 1024**2,
            "oom_kills": int(values["oom_kill"]),
            "swap_peak_mib": None
            if values["memory.swap.peak"] == "n/a"
            else int(values["memory.swap.peak"]) / 1024**2,
            "restart_count": inspected["RestartCount"],
            "oom_killed": inspected["State"]["OOMKilled"],
            "running": inspected["State"]["Running"],
        }
    except (KeyError, IndexError, TypeError, ValueError) as error:
        raise ExecutionError(f"Invalid Docker/cgroup resource data: {error}") from error


def health(port):
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(f"http://127.0.0.1:{port}/health", timeout=5) as response:
        data = json.load(response)
        if (
            response.status != 200
            or data.get("status") != "ok"
            or data.get("db") != "ok"
        ):
            raise ExecutionError("The app is not healthy")


def compose_config(project, output, port, reuse_images=False):
    config = json.loads(
        command(
            [
                "docker",
                "compose",
                "-p",
                project,
                "-f",
                str(HERE / "compose.yaml"),
                "config",
                "--format",
                "json",
            ]
        )
    )
    config["services"].pop("test", None)
    for service in ("app", "seed"):
        config["services"][service]["image"] = (
            command(
                [
                    "docker",
                    "image",
                    "inspect",
                    f"twelve-dollar-java-{service}:latest",
                    "--format",
                    "{{.Id}}",
                ]
            ).strip()
            if reuse_images
            else f"{project}-{service}:latest"
        )
    # This local baseline forbids app swap, but does not emulate the entire VM.
    config["services"]["app"]["memswap_limit"] = config["services"]["app"]["mem_limit"]
    config["services"]["nginx"]["ports"][0]["published"] = str(port)
    config["services"]["k6"] = {
        "image": K6_IMAGE,
        "profiles": ["bench"],
        "user": f"{os.getuid()}:{os.getgid()}",
        "networks": {"default": None},
        "environment": {
            "BASE_URL": "http://nginx",
            "TOKENS": "/data/tokens.json",
            "NO_PROXY": "*",
            "no_proxy": "*",
        },
        "volumes": [
            {
                "type": "volume",
                "source": "database",
                "target": "/data",
                "read_only": True,
            },
            {
                "type": "bind",
                "source": str(ROOT / "bench"),
                "target": "/bench",
                "read_only": True,
            },
            {"type": "bind", "source": str(output / "k6"), "target": "/results"},
        ],
    }
    filename = output / "compose.json"
    (output / "k6").mkdir()
    filename.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    return ["docker", "compose", "-p", project, "-f", str(filename)]


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output", type=Path, help="New directory for reports and raw evidence"
    )
    parser.add_argument(
        "--reuse-images",
        action="store_true",
        help="Use existing app/seed images instead of building source",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=3001,
        help="Localhost health-check port, default 3001",
    )
    parser.add_argument("--start-vus", type=int, default=2500)
    parser.add_argument("--max-vus", type=int, default=20000)
    parser.add_argument("--step", type=int, default=250)
    parser.add_argument("--warmup-vus", type=int, default=1000)
    parser.add_argument("--warmup-seconds", type=int, default=120)
    parser.add_argument("--probe-seconds", type=int, default=60)
    parser.add_argument("--confirm-seconds", type=int, default=300)
    parser.add_argument(
        "--confirm-runs",
        type=int,
        default=1,
        help="Consecutive confirmations required at a candidate",
    )
    parser.add_argument("--ramp-seconds", type=int, default=60)
    parser.add_argument(
        "--budget-seconds",
        type=int,
        default=3600,
        help="Load-search time budget, excluding build/startup",
    )
    args = parser.parse_args(argv)
    for key, value in vars(args).items():
        if isinstance(value, int) and not isinstance(value, bool) and value <= 0:
            parser.error(f"--{key.replace('_', '-')} must be positive")
    if (
        args.port > 65535
        or args.start_vus > args.max_vus
        or args.start_vus % args.step
        or args.max_vus % args.step
    ):
        parser.error(
            "Port must be <= 65535; start/max users must be step multiples, with start <= max"
        )
    return args


def main(argv=None):
    args = arguments(argv)
    suffix = uuid.uuid4().hex[:10]
    project = "twelve-dollar-benchmark-" + suffix
    output = (
        args.output
        or HERE
        / "benchmark-results"
        / (
            datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ-")
            + suffix
        )
    ).resolve()
    # Never overwrite evidence or accept an existing directory owned by another run.
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        print(f"Cannot create a new output directory: {error}", file=sys.stderr)
        return 2
    state = {
        "status": "starting",
        "runs": [],
        "confirmed_vus": None,
        "environment": {
            "host": platform.platform(),
            "host_logical_cpus": os.cpu_count(),
            "project": project,
            "k6_image": K6_IMAGE,
            "settings": {
                key: str(value) if isinstance(value, Path) else value
                for key, value in vars(args).items()
            },
        },
    }
    report(output, state)
    compose = None
    exit_code = 0

    def interrupted(_signum, _frame):
        raise KeyboardInterrupt

    previous_handlers = {
        sig: signal.signal(sig, interrupted)
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
    }
    try:
        compose = compose_config(project, output, args.port, args.reuse_images)
        if not args.reuse_images:
            print(f"Building source. Logs: {output / 'build.log'}", flush=True)
            command(
                compose + ["build", "app", "seed"],
                log=output / "build.log",
                timeout=1800,
            )
        try:
            command(["docker", "image", "inspect", K6_IMAGE])
        except ExecutionError:
            command(
                compose + ["--profile", "bench", "pull", "k6"],
                log=output / "pull.log",
                timeout=180,
            )
        command(
            compose
            + [
                "up",
                "--no-build",
                "--pull",
                "never",
                "-d",
                "--wait",
                "--wait-timeout",
                "90",
            ],
            log=output / "startup.log",
        )
        health(args.port)
        container = command(compose + ["ps", "-q", "app"]).strip()
        inspected = json.loads(command(["docker", "inspect", container]))[0]
        state["environment"].update(
            {
                "app_image_id": inspected["Image"],
                "app_cpu_limit": inspected["HostConfig"]["NanoCpus"] / 1e9,
                "app_memory_limit_mib": inspected["HostConfig"]["Memory"] / 1024**2,
                "app_swap_allowed": inspected["HostConfig"]["MemorySwap"]
                != inspected["HostConfig"]["Memory"],
                "load_path": "k6 -> Compose bridge -> Nginx -> app",
                "workload_sha256": hashlib.sha256(
                    (ROOT / "bench/load.js").read_bytes()
                ).hexdigest(),
            }
        )
        deadline = time.monotonic() + args.budget_seconds
        if state["environment"]["app_swap_allowed"]:
            raise ExecutionError(
                "The benchmark app's Docker configuration permits swap"
            )
        state["status"] = "running"

        def run(phase, vus):
            duration = (
                args.warmup_seconds
                if phase == "warmup"
                else (
                    args.confirm_seconds if phase == "confirm" else args.probe_seconds
                )
            )
            expected = (
                args.ramp_seconds
                + duration
                + RAMP_DOWN_SECONDS
                + GRACEFUL_STOP_SECONDS
                + STARTUP_ALLOWANCE_SECONDS
            )
            remaining = deadline - time.monotonic()
            if remaining < expected:
                raise BudgetExpired("Time budget cannot fit the next complete run")
            identifier = len(state["runs"]) + 1
            log = f"run-{identifier:03d}.log"
            summary = f"run-{identifier:03d}.json"
            events = f"run-{identifier:03d}.events.jsonl"
            item = {
                "id": identifier,
                "phase": phase,
                "vus": vus,
                "hold_seconds": duration,
                "started_at": now(),
                "status": "running",
                "log": log,
            }
            state["runs"].append(item)
            report(output, state)
            print(
                f"Run {identifier}: {phase}, {vus} users, {duration}s hold", flush=True
            )
            started = time.monotonic()
            try:
                if (
                    hashlib.sha256((ROOT / "bench/load.js").read_bytes()).hexdigest()
                    != state["environment"]["workload_sha256"]
                ):
                    raise ExecutionError("The workload changed during the search")
                before = app_resources(compose, container)
                code = command(
                    compose
                    + [
                        "--profile",
                        "bench",
                        "run",
                        "--no-deps",
                        "--pull",
                        "never",
                        "--rm",
                        "--name",
                        f"{project}-k6",
                        "-e",
                        f"VUS={vus}",
                        "-e",
                        f"DURATION={duration}s",
                        "-e",
                        f"RAMP_UP={args.ramp_seconds}s",
                        "k6",
                        "run",
                        "--quiet",
                        "--no-usage-report",
                        "--new-machine-readable-summary=false",
                        "--log-format=json",
                        f"--log-output=file=/results/{events}",
                        f"--summary-export=/results/{summary}",
                        "/bench/load.js",
                    ],
                    log=output / log,
                    timeout=max(1, deadline - time.monotonic()),
                    check=False,
                )
                item["exit_code"] = code
                item["events"] = "k6/" + events
                if not (output / "k6" / events).is_file():
                    raise ExecutionError(
                        f"k6 exited {code} without its event log; see {output / log}"
                    )
                validate_events(output / "k6" / events, code)
                if not (output / "k6" / summary).is_file():
                    raise ExecutionError(f"No k6 summary produced; exit code {code}")
                item["summary"] = "k6/" + summary
                item.update(
                    parse_summary(
                        json.loads((output / "k6" / summary).read_bytes()),
                        code,
                        expected_vus=vus,
                        minimum_seconds=args.ramp_seconds
                        + duration
                        + RAMP_DOWN_SECONDS,
                    )
                )
                after = app_resources(compose, container)
                wall = time.monotonic() - started
                item["resources"] = {
                    **after,
                    "cpu_percent": (after["cpu_usec"] - before["cpu_usec"])
                    / (wall * 1e6)
                    * 100,
                }
                if (
                    not after["running"]
                    or after["oom_killed"]
                    or after["oom_kills"] > before["oom_kills"]
                    or after["restart_count"] != before["restart_count"]
                    or (after.get("swap_peak_mib") or 0) > 0
                ):
                    raise ExecutionError(
                        "App stopped, restarted, or was OOM-killed during the run"
                    )
                health(args.port)
            except KeyboardInterrupt:
                item.update(status="interrupted", error="Interrupted during this run")
                raise
            except subprocess.TimeoutExpired as error:
                item.update(
                    status="budget exhausted",
                    error="Load run exceeded the remaining search budget",
                )
                raise BudgetExpired(item["error"]) from error
            except (
                ExecutionError,
                OSError,
                ValueError,
                KeyError,
                IndexError,
                TypeError,
                AttributeError,
                subprocess.SubprocessError,
            ) as error:
                item.update(status="execution error", error=str(error))
                raise ExecutionError(str(error)) from error
            except Exception as error:
                item.update(
                    status="execution error", error=f"{type(error).__name__}: {error}"
                )
                (output / f"run-{identifier:03d}.traceback.txt").write_text(
                    traceback.format_exc(), encoding="utf-8"
                )
                raise ExecutionError(item["error"]) from error
            finally:
                item["wall_seconds"] = time.monotonic() - started
                report(output, state)
            print(
                f"  {item['status']}: p95={item['metrics']['p95_ms']:.2f}ms, p99={item['metrics']['p99_ms']:.2f}ms, errors={item['metrics']['error_percent']:.3f}%",
                flush=True,
            )
            return item["status"] == "passed"

        run("warmup", args.warmup_vus)
        confirmed, capped = search(
            run, args.start_vus, args.max_vus, args.step, args.confirm_runs
        )
        state["confirmed_vus"] = confirmed
        state["status"] = (
            "capped"
            if capped
            else ("completed" if confirmed else "no passing candidate")
        )
        state["message"] = (
            "User cap reached without a failing probe; the confirmed result is a lower bound, not a maximum."
            if capped
            else (
                "Search and confirmation finished."
                if confirmed
                else "No candidate passed confirmation."
            )
        )
        if args.confirm_seconds < 300:
            state["message"] += (
                " Confirmation was shorter than 5 minutes; this is a smoke-test result, not a challenge-length confirmation."
            )
        if args.ramp_seconds != 60:
            state["message"] += (
                " Custom ramp duration; this result is not comparable to the default workload."
            )
        if confirmed is None:
            exit_code = 1
    except BudgetExpired as error:
        state.update(status="budget exhausted", message=str(error))
        exit_code = 2
    except KeyboardInterrupt:
        state.update(
            status="interrupted",
            message="Search interrupted. Completed-run evidence is retained.",
        )
        exit_code = 130
    except (
        ExecutionError,
        OSError,
        ValueError,
        KeyError,
        IndexError,
        TypeError,
        AttributeError,
        subprocess.SubprocessError,
    ) as error:
        state.update(status="execution error", message=str(error))
        exit_code = 2
    except Exception as error:
        LOGGER.exception("Unexpected benchmark error; evidence is retained")
        state.update(
            status="execution error",
            message=f"{type(error).__name__}: {error}; see traceback.txt",
        )
        (output / "traceback.txt").write_text(traceback.format_exc(), encoding="utf-8")
        exit_code = 2
    finally:
        for sig in previous_handlers:
            signal.signal(sig, signal.SIG_IGN)
        report(output, state)
        if compose:
            try:
                command(
                    compose + ["logs", "--no-color", "app", "nginx", "seed"],
                    log=output / "stack.log",
                    timeout=30,
                )
            except (ExecutionError, OSError, subprocess.SubprocessError):
                pass
            try:
                containers = command(
                    [
                        "docker",
                        "ps",
                        "-aq",
                        "--filter",
                        f"label=com.docker.compose.project={project}",
                        "--filter",
                        "label=com.docker.compose.service=k6",
                    ]
                ).split()
                if containers:
                    command(
                        ["docker", "rm", "--force", *containers],
                        log=output / "load-cleanup.log",
                        timeout=30,
                    )
            except (ExecutionError, OSError, subprocess.SubprocessError) as error:
                state["load_cleanup_error"] = str(error)
            try:
                command(
                    compose
                    + ["--profile", "bench", "down", "--volumes", "--remove-orphans"],
                    log=output / "cleanup.log",
                    timeout=120,
                )
            except (ExecutionError, OSError, subprocess.SubprocessError) as error:
                state["cleanup_error"] = str(error)
                state["message"] = (
                    state.get("message", "") + f" Cleanup failed: {error}"
                )
                exit_code = 2
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
