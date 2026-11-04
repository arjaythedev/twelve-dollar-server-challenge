"""Command-line entry point for developer checks and workloads"""

import argparse
import json
import subprocess
from pathlib import Path

from devtools import checks, reports, runtime


class Arguments(argparse.Namespace):
    action: str
    docker: bool
    label: str
    vus: int | None
    hold: str
    ramp: str
    generator: runtime.GeneratorKind
    file: Path | None
    json_output: bool
    reuse_server: bool
    perf: runtime.PerfMode
    client_diagnostics: bool
    telemetry: reports.TelemetryMode
    seed: int | None
    image: str | None


def workload_config(args: Arguments) -> runtime.RunConfig:
    return runtime.RunConfig(
        label=args.label,
        vus=args.vus if args.vus is not None else 5000 if args.perf != "off" else 350000,
        ramp=args.ramp,
        hold=args.hold,
        generator=args.generator,
        telemetry=args.telemetry,
        perf=args.perf,
        client_diagnostics=args.client_diagnostics,
        seed=args.seed,
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Checks, Docker benchmarks and saved results. Run with uv."
    )
    actions = parser.add_subparsers(dest="action", required=True)
    check = actions.add_parser("check", help="lint, formatting and strict typing")
    check.add_argument("--docker", action="store_true", help="also run native and HTTP tests")
    bench = actions.add_parser("bench", help="run the browsing workload in Docker")
    bench.add_argument("--label", required=True, help="unique lowercase run name")
    bench.add_argument("--vus", type=int, help="users (default: 5000 with perf, otherwise 350000)")
    bench.add_argument("--seed", type=int, help="deterministic C++ workload seed")
    bench.add_argument("--hold", default="60s", help="time at target users (default: 60s)")
    bench.add_argument("--ramp", default="30s", help="ramp-up time (default: 30s)")
    bench.add_argument(
        "--generator",
        choices=("cpp", "k6"),
        default="cpp",
        help="k6 is the pinned Docker reference",
    )
    bench.add_argument(
        "--perf", choices=("off", "user", "kernel", "all"), default="off", help="CPU sampling scope"
    )
    bench.add_argument(
        "--telemetry",
        choices=("off", "basic", "profile"),
        default="off",
        help="server telemetry mode",
    )
    bench.add_argument(
        "--client-diagnostics", action="store_true", help="native client scheduling diagnostics"
    )
    bench.add_argument(
        "--reuse-server", action="store_true", help="reuse the selected server image"
    )
    bench.add_argument("--image", help="reuse this existing server image instead of building")
    results = actions.add_parser("results", help="display saved benchmark results")
    results.add_argument("file", type=Path, nargs="?")
    results.add_argument("--json", action="store_true", dest="json_output", help="emit JSON")
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args(namespace=Arguments())
    try:
        if args.action == "check":
            checks.check(args.docker)
        elif args.action == "results":
            file = args.file
            if file is None:
                anchor = runtime.command("git", "-C", str(runtime.ROOT), "rev-parse", "HEAD")
                file = runtime.ROOT / "src" / "runs" / anchor[:7]
                document = reports.load_saved_report(file) if file.exists() else []
            else:
                document = reports.load_saved_report(file)
            if args.json_output:
                print(json.dumps(document, allow_nan=False))
            else:
                reports.show_saved_report(document)
        else:
            try:
                config = workload_config(args)
            except ValueError as error:
                parser.error(str(error))
            raise SystemExit(
                runtime.execute_workload(config, image=args.image, reuse_server=args.reuse_server)
            )
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
