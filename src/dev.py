"""Command-line entry point for developer checks and workloads"""

import argparse
import json
import subprocess
from pathlib import Path

from devtools import checks, reports, runtime

DEFAULT_RESULTS = runtime.ROOT / "src/benchmarks/results.json"


class Arguments(argparse.Namespace):
    action: str
    docker: bool
    label: str
    vus: int
    hold: str
    ramp: str
    k6: Path | None
    generator: runtime.GeneratorKind
    save: Path | None
    file: Path
    json_output: bool
    no_build: bool
    scope: reports.ProfileScope
    telemetry: reports.TelemetryMode = "off"
    seed: int | None = None
    image: str | None = None


def workload_config(args: Arguments) -> runtime.RunConfig:
    return runtime.RunConfig(
        label=args.label,
        vus=args.vus,
        ramp=args.ramp,
        hold=args.hold,
        mode="profile" if args.action == "profile" else "bench",
        generator=args.generator,
        k6=args.k6,
        telemetry=args.telemetry,
        scope=args.scope if args.action == "profile" else "user",
        seed=args.seed,
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Checks, Docker benchmarks and saved results. Run with uv."
    )
    actions = parser.add_subparsers(dest="action", required=True)
    checks = actions.add_parser("check", help="lint, formatting and strict typing")
    checks.add_argument(
        "--docker", action="store_true", help="also build Linux and run native and HTTP tests"
    )
    for action, description in (
        ("bench", "run the browsing workload in Docker"),
        ("profile", "sample native CPU stacks during the workload"),
    ):
        benchmark = actions.add_parser(action, help=description)
        benchmark.add_argument("--label", required=True, help="unique lowercase run name")
        benchmark.add_argument(
            "--vus",
            type=int,
            default=350000 if action == "bench" else 5000,
            help="users (default: %(default)s)",
        )
        benchmark.add_argument("--seed", type=int, help="deterministic C++ workload seed")
        benchmark.add_argument(
            "--hold", default="60s", help="time at target users (default: 60s; confirmation: 5m)"
        )
        benchmark.add_argument(
            "--ramp", default="30s", help="ramp-up time (default: 30s; confirmation: 60s)"
        )
        generator = benchmark.add_mutually_exclusive_group()
        generator.add_argument(
            "--generator",
            choices=("cpp", "k6"),
            default="cpp",
            help="load generator (default: cpp; k6 is the reference)",
        )
        generator.add_argument(
            "--k6", type=Path, help="native k6 executable; defaults to pinned Linux k6 in Docker"
        )
        benchmark.add_argument(
            "--save", type=Path, default=DEFAULT_RESULTS if action == "bench" else None
        )
        benchmark.add_argument(
            "--no-build", action="store_true", help="reuse existing images without rebuilding"
        )
        if action == "bench":
            benchmark.add_argument(
                "--image", help="benchmark an existing runtime image without rebuilding"
            )
        benchmark.add_argument(
            "--telemetry",
            choices=("off", "basic", "profile"),
            default="off",
            help="server telemetry build (default: off); profile also measures client scheduling",
        )
        if action == "profile":
            benchmark.add_argument("--scope", choices=("user", "kernel", "all"), default="user")
    results = actions.add_parser("results", help="display saved benchmark results")
    results.add_argument("file", type=Path, nargs="?", default=DEFAULT_RESULTS)
    results.add_argument(
        "--json", action="store_true", dest="json_output", help="emit JSON for other tools"
    )
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args(namespace=Arguments())
    try:
        if args.action == "check":
            checks.check(args.docker)
        elif args.action == "results":
            document = reports.read_json(args.file)
            if args.json_output:
                print(json.dumps(document, allow_nan=False))
            else:
                reports.show_saved_report(document)
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
            if args.seed is not None and (
                not 0 <= args.seed < 2**64 or args.k6 is not None or args.generator != "cpp"
            ):
                parser.error("--seed requires the C++ generator and an unsigned 64-bit integer")
            reports.duration_seconds(args.ramp)
            reports.duration_seconds(args.hold)
            destination = args.save
            saved = (
                reports.parse_results(reports.read_json(destination))
                if not profiling and destination is not None and destination.exists()
                else []
            )
            if any(row["name"] == args.label for row in saved):
                parser.error("run name already exists; choose a new --label")
            output = runtime.ROOT / "results" / args.label
            output.mkdir(parents=True, exist_ok=False)
            runtime_image = args.image or runtime.image_name(telemetry=args.telemetry)
            if not args.no_build and args.image is None:
                runtime.build(telemetry=args.telemetry)
                if profiling:
                    runtime.build(profiler=True, telemetry=args.telemetry)
            if args.telemetry != "off":
                print(
                    f"Server telemetry: {args.telemetry}; collection overhead is included.",
                    flush=True,
                )
            code = runtime.run_workload(workload_config(args), output, image=runtime_image)
            run_report = reports.summarize(output)
            result = run_report.result
            reports.show_results([result])
            workload = run_report.generator
            if profiling:
                reports.save_profile_result(output, result, workload, destination)
            else:
                if destination is None:
                    raise ValueError("a result file is required for benchmarks")
                destination.parent.mkdir(parents=True, exist_ok=True)
                reports.append_result(destination, result)
            if args.telemetry == "profile":
                reports.show_diagnostics(workload)
            if args.telemetry != "off":
                reports.show_telemetry(reports.read_json(output / "telemetry.json"))
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
