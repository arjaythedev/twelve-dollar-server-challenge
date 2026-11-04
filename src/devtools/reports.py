"""JSON reports, performance parsing and saved result rendering"""

import hashlib
import json
import math
import re
from collections import Counter
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from statistics import median
from typing import Literal, Never, NotRequired, TypedDict, cast

PROFILE_EVENT = "cpu-clock:u"
PROFILE_FREQUENCY = 997
PROFILE_STACK_BYTES = 8192
NETWORK_COUNTERS = frozenset(
    {
        "InSegs",
        "OutSegs",
        "RetransSegs",
        "ListenOverflows",
        "ListenDrops",
        "TCPTimeouts",
        "TCPSynRetrans",
        "TCPSpuriousRTOs",
        "TCPBacklogDrop",
        "TCPRcvQDrop",
        "TCPZeroWindowDrop",
        "TCPWqueueTooBig",
    }
)


PROFILE_STAGES = {
    "database_and_rows": "SQL / row assembly",
    "transport_and_other": "Transport / other",
    "response_json": "Response JSON",
    "request_json": "Request JSON",
    "auth": "Auth / JWT JSON",
    "unknown": "Unknown",
    "kernel_network": "Network / sockets",
    "kernel_filesystem": "Files / locks / page cache",
    "kernel_io_uring": "io_uring machinery",
    "kernel_scheduler": "Scheduling / timers",
    "kernel_other": "Other kernel work",
}


type JSONValue = bool | int | float | str | list[JSONValue] | dict[str, JSONValue] | None


type ProfileScope = Literal["user", "kernel", "all"]

type TelemetryMode = Literal["off", "basic", "profile"]

TELEMETRY_SOURCES = ("transport", "database", "checkpoint")


class TelemetryCollection(TypedDict):
    kind: Literal["server_telemetry"]
    schema_version: Literal[1]
    mode: TelemetryMode | None
    expected_sources: list[str]
    missing_sources: list[str]
    complete: bool
    reports: list[dict[str, JSONValue]]
    overwritten_windows: int
    dropped_events: int
    collection_errors: list[str]


class Result(TypedDict):
    date: str
    name: str
    runtime: str
    load_generator: str
    generator: NotRequired[str]
    client_sampled_peak_bytes: NotRequired[int]
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
    syscalls_cpu_ns: NotRequired[dict[str, int]]


@dataclass(frozen=True)
class RunReport:
    result: Result
    generator: dict[str, JSONValue]


def profile_event(scope: ProfileScope) -> str:
    return {"user": PROFILE_EVENT, "kernel": "cpu-clock:k", "all": "cpu-clock"}[scope]


def reject_constant(value: str) -> Never:
    raise ValueError(f"invalid JSON constant: {value}")


def decode_json(data: bytes) -> JSONValue:
    return cast(JSONValue, json.loads(data.decode("utf-8"), parse_constant=reject_constant))


def write_json(path: Path, value: object, *, compact: bool = False) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    encoded = json.dumps(
        value,
        indent=None if compact else 2,
        separators=(",", ":") if compact else None,
        allow_nan=False,
    )
    temporary.write_text(encoded + "\n", encoding="utf-8", newline="\n")
    temporary.replace(path)


def telemetry_counter(record: dict[str, JSONValue], key: str) -> int:
    value = record[key]
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"{key}: expected an unsigned integer")
    return value


def parse_telemetry(
    data: str, *, expected_mode: TelemetryMode | None = None
) -> TelemetryCollection:
    records: list[dict[str, JSONValue]] = []
    sources: set[str] = set()
    mode = expected_mode
    overwritten = 0
    dropped = 0
    for line_number, line in enumerate(data.splitlines(), 1):
        if not line.strip():
            continue
        try:
            record = mapping(decode_json(line.encode("utf-8")))
            if type(record.get("schema_version")) is not int or record["schema_version"] != 1:
                raise ValueError("unsupported telemetry schema version")
            source = text(record, "source")
            if source not in TELEMETRY_SOURCES or source in sources:
                raise ValueError("unknown or duplicate telemetry source")
            selected = text(record, "mode")
            if selected != "basic" and selected != "profile":
                raise ValueError("invalid telemetry mode")
            if mode is not None and mode != selected:
                raise ValueError("mixed or unexpected telemetry mode")
            mode = selected
            mapping(record["totals"])
            if not isinstance(record["windows"], list):
                raise ValueError("telemetry windows must be an array")
            overwritten += telemetry_counter(record, "overwritten_windows")
            dropped += telemetry_counter(record, "dropped_events")
            sources.add(source)
            records.append(record)
        except (ValueError, KeyError) as error:
            raise ValueError(f"telemetry line {line_number}: {error}") from error
    missing = [source for source in TELEMETRY_SOURCES if source not in sources]
    return TelemetryCollection(
        kind="server_telemetry",
        schema_version=1,
        mode=mode,
        expected_sources=list(TELEMETRY_SOURCES),
        missing_sources=missing,
        complete=not missing and overwritten == 0 and dropped == 0,
        reports=records,
        overwritten_windows=overwritten,
        dropped_events=dropped,
        collection_errors=[],
    )


def parse_counters(values: str) -> dict[str, JSONValue]:
    rows, process = values.rsplit("process_stat ", 1)
    fields = process[process.rindex(")") + 2 :].split()
    result: dict[str, JSONValue] = {}
    network_headers: dict[str, list[str]] = {}
    for line in rows.splitlines():
        name, value, *parts = line.split()
        if name in ("Tcp:", "TcpExt:"):
            network_values = [value, *parts]
            if name not in network_headers:
                network_headers[name] = network_values
                continue
            for key, item in zip(network_headers.pop(name), network_values, strict=True):
                if key in NETWORK_COUNTERS:
                    result[f"net_{name[:-1].lower()}_{key}"] = int(item)
            continue
        if name.endswith("_pressure"):
            for part in parts:
                if part.startswith("total="):
                    result[f"{name}_{value}_usec"] = int(part.removeprefix("total="))
                    break
            else:
                raise ValueError(f"missing total in {name}")
        else:
            result[name] = int(value)
    if network_headers:
        raise ValueError("incomplete TCP counter rows")
    result["main_user_ticks"] = int(fields[11])
    result["main_system_ticks"] = int(fields[12])
    return result


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


def read_json(path: Path) -> JSONValue:
    return decode_json(path.read_bytes())


def load_saved_report(path: Path) -> JSONValue:
    if path.is_file():
        if path.name == "summary.json" and (path.parent / "manifest.json").is_file():
            return load_saved_report(path.parent)
        return read_json(path)
    manifest_path = path / "manifest.json"
    if manifest_path.is_file():
        manifest = mapping(read_json(manifest_path))
        summary = path / "summary.json"
        provenance = manifest.get("provenance")
        source = provenance.get("source") if isinstance(provenance, dict) else None
        if isinstance(source, dict):
            actual_patch = hashlib.sha256((path / "source.patch").read_bytes()).hexdigest()
            if actual_patch != source.get("patch_sha256"):
                raise ValueError(f"source.patch does not match its manifest: {path}")
        expected = manifest.get("summary_sha256")
        if (
            isinstance(expected, str)
            and hashlib.sha256(summary.read_bytes()).hexdigest() != expected
        ):
            raise ValueError(f"summary does not match its manifest: {summary}")
        return read_json(summary)
    if not path.is_dir():
        raise FileNotFoundError(path)
    manifests = sorted({*path.glob("*/manifest.json"), *path.glob("*/*/manifest.json")})
    if not manifests:
        raise ValueError(f"no saved runs in {path}")
    entries: list[JSONValue] = []
    for file in manifests:
        entries.append({"manifest": read_json(file), "summary": load_saved_report(file.parent)})
    return {"kind": "run_collection", "schema_version": 1, "entries": entries}


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
    original = row[key]
    if isinstance(original, int) and not isinstance(original, bool):
        return original
    value = number(row, key)
    if not value.is_integer():
        raise ValueError(f"{key}: expected an integer")
    return int(value)


def text(row: dict[str, JSONValue], key: str) -> str:
    value = row[key]
    if not isinstance(value, str):
        raise ValueError(f"{key}: expected text")
    return value


def parse_results(rows: JSONValue) -> list[Result]:
    if isinstance(rows, dict) and "runs" in rows:
        rows = rows["runs"]
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
        if "generator" in row:
            results[-1]["generator"] = text(row, "generator")
        if "client_sampled_peak_bytes" in row:
            results[-1]["client_sampled_peak_bytes"] = integer(row, "client_sampled_peak_bytes")
    return results


def summarize(output: Path, *, label: str | None = None) -> RunReport:
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
    result = Result(
        date=datetime.fromtimestamp((output / "run.json").stat().st_mtime, UTC).date().isoformat(),
        name=label or output.name,
        runtime=f"C++ {text(runtime, 'version')}",
        load_generator=text(run, "load_generator") if "load_generator" in run else "native",
        generator=text(run, "generator") if "generator" in run else "k6",
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
    samples = read_json(output / "samples.json")
    if not isinstance(samples, list):
        raise ValueError("client samples must be an array")
    memory_key = (
        "client_cgroup_bytes"
        if result["load_generator"] == "docker_loopback"
        else "client_rss_bytes"
    )
    peak_bytes = 0
    for value in samples:
        sample = mapping(value)
        if memory_key in sample:
            peak_bytes = max(peak_bytes, integer(sample, memory_key))
    if peak_bytes:
        result["client_sampled_peak_bytes"] = peak_bytes
    return RunReport(result, mapping(summary.get("generator", {})))


def show_results(results: list[Result]) -> None:
    rows = [
        [
            "Run",
            "Runtime",
            "Client",
            "Generator",
            "Client MiB",
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
                row.get("generator", "k6"),
                f"{row['client_sampled_peak_bytes'] / 2**20:.2f}"
                if "client_sampled_peak_bytes" in row
                else "-",
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


def profile_stage(frames: list[tuple[str, str]]) -> str:
    symbols = "\n".join(symbol for symbol, _ in frames)
    if "tds::authenticate(" in symbols or any("libcrypto" in module for _, module in frames):
        return "auth"
    if any(name in symbols for name in ("tds::quote(", "tds::integer(", "tds::PostWriter::")):
        return "response_json"
    if "tds::read_json(" in symbols or "yyjson" in symbols:
        return "request_json"
    if "sqlite3" in symbols or "tds::Database::" in symbols or "tds::Store::" in symbols:
        return "database_and_rows"
    if any(
        name in symbols
        for name in (
            "tds::Server::",
            "tds::Application::",
            "tds::http::",
            "tds::net::",
            "llhttp",
            "io_uring",
        )
    ):
        return "transport_and_other"
    return "unknown"


def kernel_stage(frames: list[tuple[str, str]]) -> str:
    symbols = [symbol for symbol, module in frames if module.startswith("[kernel")]
    if any(
        symbol.startswith(("tcp_", "udp_", "ip_", "inet_", "sock_", "sk_", "skb_", "__sock_"))
        for symbol in symbols
    ):
        return "kernel_network"
    if any(
        symbol.startswith(
            ("vfs_", "ovl_", "ext4_", "filemap_", "shmem_", "locks_", "posix_lock", "fcntl_")
        )
        or symbol in ("do_fcntl", "ksys_pread64", "ksys_pwrite64")
        for symbol in symbols
    ):
        return "kernel_filesystem"
    if any("io_uring" in symbol or symbol.startswith(("io_", "__io_")) for symbol in symbols):
        return "kernel_io_uring"
    if any(
        "schedule" in symbol or symbol.startswith(("hrtimer_", "timer_", "finish_task_switch"))
        for symbol in symbols
    ):
        return "kernel_scheduler"
    return "kernel_other" if symbols else "unknown"


def analyze_profile(stacks: str, statistics: str, *, scope: ProfileScope = "user") -> NativeProfile:
    event = profile_event(scope)
    stages: Counter[str] = Counter()
    weights: Counter[tuple[str, str]] = Counter()
    counts: Counter[tuple[str, str]] = Counter()
    syscalls: Counter[str] = Counter()
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
        kernel = scope == "kernel" or (
            scope == "all" and any(module.startswith("[kernel") for _, module in frames)
        )
        samples += 1
        unresolved += leaf[0] == "[unknown]"
        unrooted += not any(
            module.startswith("[kernel") if kernel else symbol == "main"
            for symbol, module in frames
        )
        stages[kernel_stage(frames) if kernel else profile_stage(frames)] += period
        if kernel:
            syscall = next(
                (
                    symbol.removeprefix("__x64_sys_")
                    for symbol, _ in frames
                    if symbol.startswith("__x64_sys_")
                ),
                "other",
            )
            syscalls[syscall] += period
        weights[leaf] += period
        counts[leaf] += 1

    aggregate = statistics.split(f"{event} stats:", 1)[0]
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
    profile = NativeProfile(
        event=event,
        frequency_hz=PROFILE_FREQUENCY,
        stack_bytes=0 if scope == "kernel" else PROFILE_STACK_BYTES,
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
    if scope != "user":
        profile["syscalls_cpu_ns"] = dict(syscalls)
    return profile


def show_profile(profile: dict[str, JSONValue]) -> None:
    total = number(profile, "sampled_cpu_ns")
    if total <= 0:
        raise ValueError("profile contains no sampled CPU time")
    kernel = text(profile, "event").endswith(":k")
    combined = text(profile, "event") == "cpu-clock"
    label = "CPU (user + kernel)" if combined else "Kernel CPU" if kernel else "User CPU"
    root = "expected roots" if combined else "kernel frames" if kernel else "main"
    print(
        f"{label}: {integer(profile, 'samples'):,} samples; "
        f"{integer(profile, 'unresolved_leaf_samples'):,} unresolved leaves; "
        f"{integer(profile, 'unrooted_samples'):,} stacks without {root}."
    )
    values = mapping(profile["stages_cpu_ns"])
    stages = [(stage, number(values, stage)) for stage in values]
    print("Exclusive stack-path categories:")
    for stage, weight in sorted(stages, key=lambda item: item[1], reverse=True):
        print(f"  {PROFILE_STAGES.get(stage, stage):24} {weight / total:6.1%}")
    if "syscalls_cpu_ns" in profile:
        syscalls = mapping(profile["syscalls_cpu_ns"])
        kernel_total = sum(number(syscalls, name) for name in syscalls)
        print("Syscall paths (share of kernel CPU, including nested work):")
        for name in sorted(syscalls, key=lambda name: number(syscalls, name), reverse=True):
            print(f"  {name:24} {number(syscalls, name) / kernel_total:6.1%}")
    hotspots = profile["hotspots"]
    if not isinstance(hotspots, list):
        raise ValueError("profile hotspots must be an array")
    print("Leaf CPU (self):")
    for value in hotspots[:8]:
        row = mapping(value)
        print(f"  {number(row, 'cpu_ns') / total:6.1%}  {text(row, 'symbol')[:100]}")


def show_latency_breakdown(workload: dict[str, JSONValue]) -> None:
    for key, title in (
        ("endpoint_duration_ms", "Endpoint latency"),
        ("phase_duration_ms", "Phase latency"),
    ):
        groups = mapping(workload.get(key, {}))
        if not groups:
            continue
        print(f"{title} (ms):")
        for name, value in groups.items():
            row = mapping(value)
            print(
                f"  {name:10} n={integer(row, 'count'):8}  "
                f"p95={number(row, 'p95'):8.3f}  p99={number(row, 'p99'):8.3f}"
            )


def show_group_commits(report: dict[str, JSONValue]) -> None:
    for key, title in (("rows", "Mixed writes"), ("valid_rows", "Valid writes")):
        values = report.get(key, [])
        if not isinstance(values, list):
            raise ValueError("group-commit samples must be an array")
        groups: dict[tuple[str, int], list[dict[str, JSONValue]]] = {}
        for value in values:
            row = mapping(value)
            group = (text(row, "mode"), integer(row, "batch"))
            groups.setdefault(group, []).append(row)
        if not groups:
            continue
        print(f"{title}: write-kernel medians, inline checkpoints, no HTTP timing")
        print("Mode              Batch  CPU us/write  Wall us/write  WAL frames  Commits")
        for (mode, batch), rows in groups.items():
            print(
                f"{mode:18} {batch:5}  "
                f"{median(number(row, 'cpu_us_per_job') for row in rows):12.3f}  "
                f"{median(number(row, 'wall_us_per_job') for row in rows):13.3f}  "
                f"{median(integer(row, 'wal_appended_frames') for row in rows):10.0f}  "
                f"{median(integer(row, 'commits') for row in rows):7.0f}"
            )


def worst_client_windows(workload: dict[str, JSONValue]) -> list[dict[str, JSONValue]]:
    diagnostics = mapping(workload.get("diagnostics", {}))
    values = diagnostics.get("windows", [])
    if not isinstance(values, list):
        raise ValueError("diagnostic windows must be an array")
    windows = [mapping(value) for value in values]
    if not windows:
        return []
    ranked = sorted(
        windows,
        key=lambda row: number(mapping(row["request_duration_ms"]), "p99_upper_ms"),
        reverse=True,
    )[:5]
    maximum = max(windows, key=lambda row: number(mapping(row["request_duration_ms"]), "max_ms"))
    if maximum not in ranked:
        ranked.append(maximum)

    return ranked


def show_diagnostics(workload: dict[str, JSONValue]) -> None:
    ranked = worst_client_windows(workload)
    if not ranked:
        return
    print("Worst client windows (approximate p99 bounds, ms):")
    print("Window s     Requests         p99 bounds        Max  Timer max  epoll max  Poll gap")
    for row in ranked:
        request = mapping(row["request_duration_ms"])
        timer = mapping(row["timer_overdue_ms"])
        epoll = mapping(row["epoll_service_lag_ms"])
        poll_gap = (
            f"{number(row, 'epoll_repoll_max_ms'):8.3f}"
            if "epoll_repoll_max_ms" in row
            else "     n/a"
        )
        print(
            f"{number(row, 'start_s'):4.0f}-{number(row, 'end_s'):<4.0f}  "
            f"{integer(request, 'count'):9}  "
            f"{number(request, 'p99_lower_ms'):9.3f}-{number(request, 'p99_upper_ms'):<9.3f}  "
            f"{number(request, 'max_ms'):9.3f}  "
            f"{number(timer, 'max_ms'):9.3f}  {number(epoll, 'max_ms'):9.3f}  {poll_gap}"
        )


def show_telemetry_probe(report: dict[str, JSONValue]) -> None:
    medians = mapping(report["medians"])
    before = mapping(medians["before"])
    after = mapping(medians["after"])
    reduction = mapping(report["reduction_percent"])
    print("Telemetry hooks (thread CPU per call)")
    print("Path                        Before ns   After ns  Reduction")
    for key in before:
        label = key.removesuffix("_ns_per_call").replace("_", " ")
        print(
            f"{label:26}  {number(before, key):9.2f}  {number(after, key):9.2f}"
            f"  {number(reduction, key):8.2f}%"
        )


def show_saved_report(document: JSONValue) -> None:
    if isinstance(document, list):
        show_results(parse_results(document))
        return
    report = mapping(document)
    kind = report.get("kind")
    canonical = kind in ("workload", "probe", "comparison")
    if canonical:
        if report.get("schema_version") != 1:
            raise ValueError("unsupported run summary version")
        print(f"Run: {report.get('label', '')} ({text(report, 'status')})")
        instruments = report.get("instruments")
        if isinstance(instruments, dict) and kind == "workload":
            print(
                f"Telemetry: {instruments.get('server_telemetry')}; "
                f"client diagnostics: {instruments.get('client_diagnostics')}; "
                f"perf: {instruments.get('perf')}"
            )
    if kind in ("probe", "comparison"):
        measurement = report.get("measurement")
        if isinstance(measurement, dict):
            if measurement.get("kind") in ("telemetry_gate_probe", "telemetry_hook_probe"):
                show_telemetry_probe(measurement)
            else:
                print(json.dumps(measurement, indent=2, allow_nan=False))
        elif measurement is not None or report.get("status") == "passed":
            raise ValueError("successful probe requires a measurement object")
    elif kind == "run_collection":
        values = report.get("entries")
        if report.get("schema_version") != 1 or not isinstance(values, list):
            raise ValueError("unsupported run collection")
        print("Run                                      Status       Kind")
        for value in values:
            manifest = mapping(mapping(value)["manifest"])
            print(
                f"{text(manifest, 'label'):40} {text(manifest, 'status'):12} {text(manifest, 'kind')}"
            )
    elif kind == "raw_archive":
        print(
            f"{text(report, 'name')}: {integer(report, 'file_count')} files, "
            f"{integer(report, 'size_bytes')} bytes"
        )
    elif kind in ("telemetry_gate_probe", "telemetry_hook_probe"):
        show_telemetry_probe(report)
    elif kind == "server_telemetry":
        show_telemetry(report)
    elif "runs" in report:
        show_results(parse_results(report))
    elif "rows" in report:
        show_group_commits(report)
    elif kind == "workload" or any(
        key in report for key in ("profile", "generator", "load_generator")
    ):
        benchmark = report.get("benchmark")
        if isinstance(benchmark, dict):
            show_results(parse_results([benchmark]))
        elif canonical:
            print("No complete HTTP measurements available")
        if "profile" in report:
            show_profile(mapping(report["profile"]))
        workload = mapping(report.get("load_generator", report.get("generator", {})))
        show_latency_breakdown(workload)
        show_diagnostics(workload)
        telemetry = report.get("server_telemetry")
        if isinstance(telemetry, dict):
            show_telemetry(telemetry)
    else:
        raise ValueError("unsupported saved report; use --json to inspect the document")
    if report.get("error"):
        print(f"Error: {report['error']}")
    errors = report.get("secondary_errors", [])
    if isinstance(errors, list):
        for error in errors:
            print(f"Collection: {error}")


def show_telemetry(document: JSONValue) -> None:
    report = mapping(document)
    if "reports" in report:
        report = telemetry_summary(report)
    values = report["owners"]
    if not isinstance(values, list):
        raise ValueError("server telemetry owners must be an array")
    status = "complete" if report["complete"] is True else "incomplete"
    print(f"Server telemetry ({report['mode']}): {status}, {len(values)}/3 owners")
    print("Source         Windows  Overwritten  Dropped events")
    for value in values:
        owner = mapping(value)
        print(
            f"{text(owner, 'source'):13}  {integer(owner, 'window_count'):7}  "
            f"{telemetry_counter(owner, 'overwritten_windows'):11}  "
            f"{telemetry_counter(owner, 'dropped_events'):14}"
        )
    missing = report["missing_sources"]
    errors = report["collection_errors"]
    if not isinstance(missing, list) or not isinstance(errors, list):
        raise ValueError("server telemetry status must contain arrays")
    for source in missing:
        print(f"Missing owner: {source}")
    for error in errors:
        print(f"Collection error: {error}")
    for value in values:
        owner = mapping(value)
        source = text(owner, "source")
        show_metrics(mapping(owner.get("metrics", {})), prefix=source)
        counters = mapping(owner.get("counters", {}))
        selected = ("write_commands", "commits", "checkpoint_busy", "checkpoint_failures")
        if any(counters.get(key, 0) for key in selected):
            print(
                f"  {source}: "
                + ", ".join(f"{key}={counters[key]}" for key in selected if key in counters)
            )
        wal = mapping(owner.get("wal", {}))
        if wal:
            print(f"  {source} WAL: " + ", ".join(f"{key}={value}" for key, value in wal.items()))


def show_metrics(metrics: dict[str, JSONValue], *, prefix: str) -> None:
    for name, value in metrics.items():
        if not isinstance(value, dict):
            continue
        label = f"{prefix}.{name}"
        if "mean" not in value:
            show_metrics(value, prefix=label)
            continue
        unit = text(value, "unit")
        scale = 1000 if unit == "ns" else 1
        print(
            f"  {label}: n={integer(value, 'count'):,}, "
            f"mean={number(value, 'mean') / scale:.3f}, "
            f"max={number(value, 'max') / scale:.3f} {'us' if unit == 'ns' else unit}"
        )


def workload_summary(workload: dict[str, JSONValue]) -> dict[str, JSONValue]:
    keys = (
        "name",
        "version",
        "seed",
        "vus",
        "passed",
        "checks_passed",
        "workflow_errors",
        "target_concurrency_reached",
        "http_response_coverage_complete",
        "peak_live_connections",
        "hold_peak_live_connections",
        "endpoint_requests",
        "endpoint_duration_ms",
        "phase_duration_ms",
    )
    summary = {key: workload[key] for key in keys if key in workload}
    if "diagnostics" in workload:
        diagnostics = mapping(workload["diagnostics"])
        selected: list[JSONValue] = []
        for window in worst_client_windows(workload):
            selected.append(
                {
                    key: {name: item for name, item in value.items() if name != "histogram"}
                    if isinstance(value, dict)
                    else value
                    for key, value in window.items()
                }
            )
        diagnostic_summary: dict[str, JSONValue] = {
            key: diagnostics[key]
            for key in (
                "workload_start_ns",
                "window_seconds",
                "windows_retained",
                "windows_overwritten",
                "dropped_late_samples",
            )
            if key in diagnostics
        }
        diagnostic_summary["selection"] = "worst_p99_and_max"
        diagnostic_summary["windows"] = selected
        summary["diagnostics"] = diagnostic_summary
    return summary


def metric_summary(metrics: dict[str, JSONValue]) -> dict[str, JSONValue]:
    summary: dict[str, JSONValue] = {}
    for name, value in metrics.items():
        if isinstance(value, list):
            summary[name] = [metric_summary(mapping(item)) for item in value]
        elif not isinstance(value, dict):
            summary[name] = value
        elif "buckets" in value:
            count = integer(value, "count")
            if count:
                summary[name] = {
                    "unit": text(value, "unit"),
                    "count": count,
                    "mean": round(integer(value, "total") / count, 3),
                    "max": integer(value, "max"),
                }
        else:
            summary[name] = metric_summary(value)
    return summary


def telemetry_summary(document: JSONValue) -> dict[str, JSONValue]:
    report = mapping(document)
    summary = {
        key: report[key]
        for key in (
            "mode",
            "complete",
            "missing_sources",
            "overwritten_windows",
            "dropped_events",
            "collection_errors",
        )
    }
    summary["scope"] = "server_lifetime"
    owners: list[JSONValue] = []
    values = report["reports"]
    if not isinstance(values, list):
        raise ValueError("server telemetry reports must be an array")
    for value in values:
        owner = mapping(value)
        totals = mapping(owner["totals"])
        windows = owner["windows"]
        if not isinstance(windows, list):
            raise ValueError("server telemetry windows must be an array")
        row = {key: owner[key] for key in ("source", "overwritten_windows", "dropped_events")}
        row["window_count"] = len(windows)
        compact = metric_summary(totals)
        for key in ("counters", "wal", "wal_advice", "metrics"):
            if key in totals:
                row[key] = compact[key]
        rings = compact.get("rings", [])
        if not isinstance(rings, list):
            raise ValueError("server telemetry rings must be an array")
        if rings:
            row["rings"] = rings
        owners.append(row)
    summary["owners"] = owners
    return summary


def run_summary(
    output: Path, *, label: str, telemetry: TelemetryMode = "off"
) -> dict[str, JSONValue]:
    report: dict[str, JSONValue] = {}
    errors: list[JSONValue] = []
    required = ("run.json", "k6-summary.json", "environment.json", "samples.json")
    missing = [name for name in required if not (output / name).is_file()]
    if missing:
        errors.append("Missing workload artifacts: " + ", ".join(missing))
    else:
        try:
            measured = summarize(output, label=label)
            report["benchmark"] = cast(JSONValue, measured.result)
            if measured.generator:
                report["load_generator"] = workload_summary(measured.generator)
        except (OSError, ValueError, KeyError) as error:
            errors.append(f"Workload summary: {error}")
    artifacts = (
        ("perf-analysis.json", "profile"),
        ("window.json", "window"),
        ("telemetry.jsonl", "server_telemetry"),
        ("collection-errors.json", "collection_errors"),
    )
    for filename, key in artifacts:
        path = output / filename
        if not path.is_file():
            if key == "server_telemetry" and telemetry != "off":
                errors.append("requested server telemetry export is missing")
            continue
        try:
            if key == "server_telemetry":
                raw = parse_telemetry(path.read_text(encoding="utf-8"), expected_mode=telemetry)
                report[key] = telemetry_summary(cast(JSONValue, raw))
            elif key == "collection_errors":
                values = read_json(path)
                if not isinstance(values, list):
                    raise ValueError("collection errors must be an array")
                errors.extend(values)
            else:
                value = mapping(read_json(path))
                report[key] = {
                    name: item for name, item in value.items() if not name.startswith("cgroup_")
                }
        except (OSError, ValueError, KeyError) as error:
            errors.append(f"{filename}: {error}")
    report["collection_errors"] = errors
    return report
