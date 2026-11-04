"""Freeze build inputs and verify their exact patch without changing the checkout"""

import hashlib
import os
import shutil
import stat
import subprocess
import tempfile
from collections.abc import Generator, Mapping
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[2]
_NATIVE_SUFFIXES = frozenset(
    {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".ipp", ".s", ".asm"}
)
_EXCLUDED_DIRECTORIES = frozenset(
    {
        ".git",
        ".venv",
        ".build",
        ".cache",
        ".ruff_cache",
        ".mypy_cache",
        ".pytest_cache",
        "__pycache__",
        "CMakeFiles",
    }
)
_ROOT_INPUTS = frozenset({"bench/load.js", "schema.sql"})
_SRC_INPUTS = frozenset(
    {
        ".dockerignore",
        ".gitattributes",
        ".python-version",
        "CMakeLists.txt",
        "Dockerfile",
        "dev.py",
        "pyproject.toml",
        "uv.lock",
        "docker/seccomp.json",
    }
)
_SRC_DIRECTORIES = frozenset(
    {"cmake", "server", "load", "devtools", "tests", "benchmarks", "docker"}
)
_GIT_ENVIRONMENT = frozenset(
    {
        "GIT_DIR",
        "GIT_WORK_TREE",
        "GIT_INDEX_FILE",
        "GIT_OBJECT_DIRECTORY",
        "GIT_ALTERNATE_OBJECT_DIRECTORIES",
        "GIT_COMMON_DIR",
        "GIT_PREFIX",
    }
)


@dataclass(frozen=True)
class SourceFile:
    path: str
    mode: str
    data: bytes = field(repr=False)

    @property
    def sha256(self) -> str:
        return hashlib.sha256(self.data).hexdigest()


@dataclass(frozen=True)
class SourceSnapshot:
    """Own a captured repository subset until its capture context exits

    Build only from ``context``; callers must not modify the owned temporary files
    ``files`` and ``patch`` retain the exact captured bytes independently of disk
    """

    root: Path
    context: Path
    base_commit: str
    patch_sha256: str
    source_sha256: str
    patch: bytes = field(repr=False)
    files: tuple[SourceFile, ...] = field(repr=False)

    def write_patch(self, destination: Path) -> None:
        destination.write_bytes(self.patch)


def _git(
    root: Path,
    *arguments: str,
    data: bytes | None = None,
    environment: Mapping[str, str] | None = None,
) -> bytes:
    executable = shutil.which("git")
    if executable is None:
        raise RuntimeError("Git is required to capture build inputs")
    variables = {key: value for key, value in os.environ.items() if key not in _GIT_ENVIRONMENT}
    if environment is not None:
        variables.update(environment)
    result = subprocess.run(  # noqa: S603 - fixed Git executable, arguments, and no shell
        [executable, "-C", str(root), *arguments],
        input=data,
        capture_output=True,
        check=True,
        env=variables,
    )
    return result.stdout


def _is_input(name: str) -> bool:
    path = PurePosixPath(name)
    if any(part in _EXCLUDED_DIRECTORIES for part in path.parts):
        return False
    if path.name == ".env" or path.name.startswith(".env."):
        return False
    if path.suffix.lower() in {".pyc", ".pyo", ".o", ".obj", ".a", ".so", ".dll", ".exe"}:
        return False
    if name in _ROOT_INPUTS:
        return True
    if len(path.parts) < 2 or path.parts[0] != "src":
        return False
    relative = PurePosixPath(*path.parts[1:])
    if relative.as_posix() in _SRC_INPUTS:
        return True
    if len(relative.parts) == 1:
        return relative.suffix == ".sh" or relative.name.startswith("Dockerfile.")
    directory = relative.parts[0]
    if directory in {"cmake", "server", "load"}:
        return True
    if directory == "devtools":
        return relative.suffix == ".py"
    if directory in {"tests", "benchmarks"}:
        return (
            relative.name == "CMakeLists.txt"
            or relative.suffix.lower() in _NATIVE_SUFFIXES
            or relative.suffix == ".cmake"
            or (directory == "tests" and relative.suffix == ".py")
        )
    return False


def _base_modes(root: Path, commit: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for record in _git(root, "ls-tree", "-rz", commit, "--", "src", *_ROOT_INPUTS).split(b"\0"):
        if not record:
            continue
        metadata, raw_path = record.split(b"\t", 1)
        name = os.fsdecode(raw_path)
        if _is_input(name):
            mode, kind, _ = metadata.decode("ascii").split()
            if kind != "blob" or mode not in {"100644", "100755"}:
                raise ValueError(f"Unsupported build input mode {mode}: {name}")
            result[name] = mode
    return result


def _index_modes(root: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for record in _git(root, "ls-files", "--stage", "-z", "--", "src", *_ROOT_INPUTS).split(b"\0"):
        if record:
            metadata, raw_path = record.split(b"\t", 1)
            mode, _, stage = metadata.decode("ascii").split()
            name = os.fsdecode(raw_path)
            if _is_input(name):
                if stage != "0":
                    raise ValueError(f"Unresolved build input conflict: {name}")
                result[name] = mode
    return result


def _walk_error(error: OSError) -> None:
    raise error


def _freeze(root: Path, base_modes: Mapping[str, str]) -> tuple[SourceFile, ...]:
    names = set(base_modes)
    index_modes = _index_modes(root)
    for directory in (root / "src", root / "bench"):
        if not directory.exists():
            continue
        for parent, directories, files in directory.walk(on_error=_walk_error):
            directories[:] = sorted(
                name
                for name in directories
                if name not in _EXCLUDED_DIRECTORIES
                and (parent != root / "src" or name in _SRC_DIRECTORIES)
                and parent != root / "bench"
            )
            for name in files:
                relative = (parent / name).relative_to(root).as_posix()
                if _is_input(relative):
                    names.add(relative)
    names.update(name for name in _ROOT_INPUTS if (root / name).exists())
    frozen: list[SourceFile] = []
    for name in sorted(names):
        path = root / name
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            continue
        if not stat.S_ISREG(metadata.st_mode):
            raise ValueError(f"Build input must be a regular file: {name}")
        # Windows lacks executable bits, so retain Git's index/base mode there
        executable = (
            index_modes.get(name, base_modes.get(name, "100644")) == "100755"
            if os.name == "nt"
            else bool(metadata.st_mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH))
        )
        frozen.append(SourceFile(name, "100755" if executable else "100644", path.read_bytes()))
    return tuple(frozen)


def _source_hash(files: tuple[SourceFile, ...]) -> str:
    digest = hashlib.sha256(b"twelve-dollar-source-v1\0")
    for source in files:
        name = os.fsencode(source.path)
        digest.update(len(name).to_bytes(8, "big"))
        digest.update(name)
        digest.update(source.mode.encode("ascii"))
        digest.update(len(source.data).to_bytes(8, "big"))
        digest.update(source.data)
    return digest.hexdigest()


def _patch(
    root: Path,
    temporary: Path,
    base_commit: str,
    base_modes: Mapping[str, str],
    files: tuple[SourceFile, ...],
) -> bytes:
    repository = temporary / "verification"
    repository.mkdir()
    object_format = _git(root, "rev-parse", "--show-object-format").decode("ascii").strip()
    _git(repository, "init", "--quiet", f"--object-format={object_format}")
    objects = _git(root, "rev-parse", "--path-format=absolute", "--git-path", "objects")
    environment = {"GIT_ALTERNATE_OBJECT_DIRECTORIES": os.fsdecode(objects).strip()}
    _git(repository, "read-tree", base_commit, environment=environment)
    blobs = temporary / "blobs"
    blobs.mkdir()
    paths: list[str] = []
    for number, source in enumerate(files):
        path = blobs / str(number)
        path.write_bytes(source.data)
        paths.append(path.as_posix())
    hashes = _git(
        repository,
        "hash-object",
        "-w",
        "--no-filters",
        "--stdin-paths",
        data="".join(path + "\n" for path in paths).encode(),
        environment=environment,
    ).splitlines()
    records = bytearray()
    current_names = {source.path for source in files}
    for name in sorted(set(base_modes) - current_names):
        records.extend(b"0 " + b"0" * len(base_commit) + b"\t" + os.fsencode(name) + b"\0")
    for source, object_hash in zip(files, hashes, strict=True):
        records.extend(
            source.mode.encode("ascii")
            + b" "
            + object_hash
            + b"\t"
            + os.fsencode(source.path)
            + b"\0"
        )
    _git(
        repository,
        "update-index",
        "-z",
        "--index-info",
        data=bytes(records),
        environment=environment,
    )
    expected_tree = _git(repository, "write-tree", environment=environment)
    patch = _git(
        repository,
        "diff",
        "--cached",
        "--binary",
        "--full-index",
        "--no-renames",
        "--no-ext-diff",
        "--no-textconv",
        "--no-color",
        "--unified=3",
        "--src-prefix=a/",
        "--dst-prefix=b/",
        "--diff-algorithm=myers",
        "--no-indent-heuristic",
        base_commit,
        "--",
        environment=environment,
    )
    _git(repository, "read-tree", base_commit, environment=environment)
    if patch:
        _git(
            repository,
            "apply",
            "--cached",
            "--binary",
            "--whitespace=nowarn",
            data=patch,
            environment=environment,
        )
    if _git(repository, "write-tree", environment=environment) != expected_tree:
        raise RuntimeError("Captured source patch did not reconstruct the frozen inputs")
    return patch


@contextmanager
def capture(root: Path = ROOT) -> Generator[SourceSnapshot]:
    """Capture HEAD and current build-input bytes, independently of staging state"""
    root = root.resolve()
    base_commit = _git(root, "rev-parse", "--verify", "HEAD^{commit}").decode("ascii").strip()
    base_modes = _base_modes(root, base_commit)
    files = _freeze(root, base_modes)
    with tempfile.TemporaryDirectory(prefix="twelve-dollar-source-") as directory:
        temporary = Path(directory)
        frozen = temporary / "source"
        context = frozen / "src"
        context.mkdir(parents=True)
        for source in files:
            destination = frozen / source.path
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(source.data)
            destination.chmod(0o755 if source.mode == "100755" else 0o644)
        patch = _patch(root, temporary, base_commit, base_modes, files)
        yield SourceSnapshot(
            root=frozen,
            context=context,
            base_commit=base_commit,
            patch_sha256=hashlib.sha256(patch).hexdigest(),
            source_sha256=_source_hash(files),
            patch=patch,
            files=files,
        )
