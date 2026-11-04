"""Build and verify pinned Docker images from frozen source inputs"""

import hashlib
import json
import subprocess
import uuid
from pathlib import Path
from typing import Literal, TypedDict

from . import reports, sources

IMAGE = "twelve-dollar-cpp:local"
PROFILER_IMAGE = "twelve-dollar-profiler:local"
GENERATOR_IMAGE = "twelve-dollar-generator:local"
GENERATOR_SOURCE_LABEL = "org.twelve-dollar.generator-source-sha256"
SOURCE_LABEL = "org.twelve-dollar.source-sha256"
PATCH_LABEL = "org.twelve-dollar.patch-sha256"
COMMIT_LABEL = "org.twelve-dollar.base-commit"
BUILD_LABEL = "org.twelve-dollar.build-spec"
K6_IMAGE = (
    "grafana/k6:1.3.0@sha256:a90b459a3768c46ad1013da53af24189f735d7112273c6ac3212ca8ed0e18656"
)
type BuildTarget = Literal["final", "profiler", "test-build", "tested-runtime", "generator"]


class ImageIdentity(TypedDict):
    image_id: str
    binary_sha256: str
    source: reports.JSONValue
    build_spec: reports.JSONValue
    toolchain_sha256: str | None
    build_sha256: str | None
    repo_digests: reports.JSONValue


def _command(*args: str, timeout: int | None = 30) -> str:
    return subprocess.run(
        args, check=True, capture_output=True, text=True, encoding="utf-8", timeout=timeout
    ).stdout.strip()


def image_command(image: ImageIdentity, executable: str, *args: str) -> str:
    return _command(
        "docker",
        "run",
        "--rm",
        "--network=none",
        f"--entrypoint={executable}",
        image["image_id"],
        *args,
    )


def _inspection(image: str) -> dict[str, reports.JSONValue]:
    return reports.mapping(
        reports.decode_json(
            _command("docker", "image", "inspect", "--format={{json .}}", image).encode()
        )
    )


def _labels(inspected: dict[str, reports.JSONValue]) -> dict[str, reports.JSONValue]:
    return reports.mapping(reports.mapping(inspected["Config"]).get("Labels") or {})


def _identity(inspected: dict[str, reports.JSONValue], binary: str) -> ImageIdentity:
    identity = reports.text(inspected, "Id")
    labels = _labels(inspected)
    values = {
        "base_commit": labels.get(COMMIT_LABEL),
        "patch_sha256": labels.get(PATCH_LABEL),
        "source_sha256": labels.get(SOURCE_LABEL),
    }
    source: dict[str, reports.JSONValue] | None = None
    if all(
        isinstance(value, str)
        and len(value) in ((40, 64) if key == "base_commit" else (64,))
        and all(character in "0123456789abcdef" for character in value)
        for key, value in values.items()
    ):
        source = values
    spec = labels.get(BUILD_LABEL)
    try:
        toolchain = _command(
            "docker",
            "run",
            "--rm",
            "--network=none",
            "--entrypoint=cat",
            identity,
            "/etc/tds-toolchain",
        )
    except subprocess.CalledProcessError:
        toolchain = None
    return ImageIdentity(
        image_id=identity,
        repo_digests=inspected.get("RepoDigests", []),
        source=source,
        build_spec=reports.decode_json(spec.encode()) if isinstance(spec, str) else None,
        toolchain_sha256=hashlib.sha256(toolchain.encode()).hexdigest() if toolchain else None,
        build_sha256=(
            hashlib.sha256((spec + "\n" + toolchain + "\n" + identity).encode()).hexdigest()
            if isinstance(spec, str) and toolchain is not None
            else None
        ),
        binary_sha256=_command(
            "docker", "run", "--rm", "--network=none", "--entrypoint=sha256sum", identity, binary
        ).split()[0],
    )


def inspect(image: str, binary: str) -> ImageIdentity:
    return _identity(_inspection(image), binary)


def image_name(*, profiler: bool = False, telemetry: reports.TelemetryMode = "off") -> str:
    base = PROFILER_IMAGE if profiler else IMAGE
    return base + ("" if telemetry == "off" else f"-{telemetry}")


def build(
    source: sources.SourceSnapshot,
    target: BuildTarget = "final",
    *,
    telemetry: reports.TelemetryMode = "off",
) -> ImageIdentity:
    print(f"Building {target} image...", flush=True)
    generator = target == "generator"
    spec: dict[str, reports.JSONValue] = {
        "platform": "linux/amd64",
        "target": target,
        "generator_only": generator,
        "sanitizers": False,
        "build_type": "Release",
    }
    if not generator:
        spec["telemetry"] = telemetry
    encoded = json.dumps(spec, sort_keys=True, separators=(",", ":"))
    alias = (
        GENERATOR_IMAGE
        if generator
        else image_name(profiler=target == "profiler", telemetry=telemetry)
        + {"test-build": "-build", "tested-runtime": "-tested"}.get(target, "")
    )
    temporary = f"twelve-dollar-build:{uuid.uuid4().hex}"
    try:
        _command(
            "docker",
            "build",
            "--progress=plain",
            "--platform=linux/amd64",
            f"--tag={temporary}",
            f"--target={target}",
            *([f"--build-arg=TDS_TELEMETRY={telemetry}"] if not generator else []),
            f"--label={SOURCE_LABEL}={source.source_sha256}",
            f"--label={PATCH_LABEL}={source.patch_sha256}",
            f"--label={COMMIT_LABEL}={source.base_commit}",
            f"--label={BUILD_LABEL}={encoded}",
            *(
                [f"--label={GENERATOR_SOURCE_LABEL}={generator_fingerprint(source.context)}"]
                if generator
                else []
            ),
            f"--file={source.context / 'Dockerfile'}",
            str(source.context),
            timeout=None,
        )
        identity = inspect(
            temporary,
            "/app/load_generator"
            if generator
            else "/build/out/server"
            if target == "test-build"
            else "/app/server",
        )
        build_hash = identity["build_sha256"]
        if build_hash is None:
            raise RuntimeError("built image does not identify its build inputs and toolchain")
        image = (
            f"{alias.split(':')[0]}:"
            f"{source.base_commit[:12]}-{source.patch_sha256[:12]}-{build_hash[:12]}"
        )
        _command("docker", "image", "tag", identity["image_id"], image)
        _command("docker", "image", "tag", identity["image_id"], alias)
        return identity
    finally:
        subprocess.run(["docker", "image", "rm", temporary], capture_output=True, check=False)


def generator_fingerprint(source: Path) -> str:
    inputs = [
        source / name
        for name in (
            "CMakeLists.txt",
            "Dockerfile",
            ".dockerignore",
            "cmake/targets.cmake",
            "cmake/dependencies.cmake",
        )
    ]
    inputs.extend(path for path in (source / "load").rglob("*") if path.is_file())
    digest = hashlib.sha256()
    for path in sorted(inputs):
        digest.update(path.relative_to(source).as_posix().encode() + b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def ensure_generator(source: sources.SourceSnapshot) -> ImageIdentity:
    fingerprint = generator_fingerprint(source.context)
    try:
        inspected = _inspection(GENERATOR_IMAGE)
        if _labels(inspected).get(GENERATOR_SOURCE_LABEL) == fingerprint:
            return _identity(inspected, "/app/load_generator")
    except subprocess.CalledProcessError:
        pass
    print("Building current native generator...", flush=True)
    return build(source, "generator")
