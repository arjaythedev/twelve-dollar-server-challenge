"""Canonical run records with explicit measured provenance and terminal summaries"""

import hashlib
import re
from copy import deepcopy
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Literal

from devtools.reports import JSONValue, mapping, write_json

type TerminalStatus = Literal["passed", "failed", "interrupted"]
type RunKind = Literal["workload", "probe", "comparison"]

_COMMIT = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})\Z")
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_LABEL = re.compile(r"[a-z0-9][a-z0-9._-]{0,95}\Z")
_RESERVED_NAMES = frozenset(
    {
        "con",
        "prn",
        "aux",
        "nul",
        *(f"com{i}" for i in range(1, 10)),
        *(f"lpt{i}" for i in range(1, 10)),
    }
)


def _timestamp() -> str:
    return datetime.now(UTC).isoformat()


def _validate_label(label: str) -> None:
    if (
        _LABEL.fullmatch(label) is None
        or label.endswith(".")
        or label.split(".", 1)[0] in _RESERVED_NAMES
    ):
        raise ValueError("run label must be a safe lowercase name of at most 96 characters")


@dataclass
class RunRecord:
    path: Path
    _manifest: dict[str, JSONValue] = field(repr=False)

    @property
    def raw(self) -> Path:
        return self.path / "raw"

    @property
    def manifest(self) -> dict[str, JSONValue]:
        return deepcopy(self._manifest)

    def _require_running(self) -> None:
        if self._manifest["status"] != "running":
            raise RuntimeError("run is already finished")

    def _record_identity(
        self, category: str, component: str, identity: dict[str, JSONValue]
    ) -> None:
        self._require_running()
        if not component:
            raise ValueError("provenance component must not be empty")
        manifest = deepcopy(self._manifest)
        provenance = mapping(manifest["provenance"])
        identities = mapping(provenance[category]) if provenance[category] is not None else {}
        identities[component] = deepcopy(identity)
        provenance[category] = identities
        write_json(self.path / "manifest.json", manifest)
        self._manifest = manifest

    def record_source(self, base_commit: str, patch: bytes, source_sha256: str) -> None:
        """Record verified tested inputs, never an unrelated observer checkout"""
        self._require_running()
        if mapping(self._manifest["provenance"])["source"] is not None:
            raise RuntimeError("tested source is already recorded")
        if _COMMIT.fullmatch(base_commit) is None:
            raise ValueError("source base_commit must be a full Git commit hash")
        if _SHA256.fullmatch(source_sha256) is None:
            raise ValueError("source_sha256 must be a full SHA-256 hash")
        manifest = deepcopy(self._manifest)
        mapping(manifest["provenance"])["source"] = {
            "base_commit": base_commit,
            "patch_sha256": hashlib.sha256(patch).hexdigest(),
            "source_sha256": source_sha256,
            "patch": "source.patch",
        }
        destination = self.path / "source.patch"
        temporary = destination.with_suffix(".patch.tmp")
        temporary.write_bytes(patch)
        temporary.replace(destination)
        write_json(self.path / "manifest.json", manifest)
        self._manifest = manifest

    def record_build(self, component: str, identity: dict[str, JSONValue]) -> None:
        self._record_identity("build", component, identity)

    def record_binary(self, component: str, identity: dict[str, JSONValue]) -> None:
        self._record_identity("binary", component, identity)

    def record_observer(self, identity: dict[str, JSONValue]) -> None:
        self._require_running()
        manifest = deepcopy(self._manifest)
        mapping(manifest["provenance"])["observer"] = deepcopy(identity)
        write_json(self.path / "manifest.json", manifest)
        self._manifest = manifest

    def finish(
        self,
        status: TerminalStatus,
        summary: dict[str, JSONValue],
        exit_code: int,
        error: str | None = None,
        secondary_errors: list[str] | None = None,
    ) -> int:
        self._require_running()
        if status not in ("passed", "failed", "interrupted"):
            raise ValueError(f"invalid terminal run status: {status}")
        finished_at = _timestamp()
        failures: list[JSONValue] = list(secondary_errors or [])
        source = mapping(self._manifest["provenance"])["source"]
        if isinstance(source, dict):
            try:
                actual = hashlib.sha256((self.path / "source.patch").read_bytes()).hexdigest()
                if actual != source["patch_sha256"]:
                    raise ValueError("source.patch does not match the tested source checksum")
            except (OSError, ValueError) as failure:
                failures.append(str(failure))
                if status == "passed":
                    status, exit_code = "failed", 1
                if error is None:
                    error = str(failure)
        terminal: dict[str, JSONValue] = {
            key: self._manifest[key]
            for key in ("schema_version", "anchor", "label", "kind", "started_at")
        }
        terminal.update(
            status=status,
            finished_at=finished_at,
            exit_code=exit_code,
            error=error,
            secondary_errors=failures,
        )
        document = deepcopy(summary)
        document.update(terminal)
        document["workload"] = deepcopy(self._manifest["workload"])
        document["instruments"] = deepcopy(self._manifest["instruments"])
        provenance = mapping(self._manifest["provenance"])
        document["provenance"] = {key: deepcopy(provenance[key]) for key in ("source", "binary")}
        manifest = deepcopy(self._manifest)
        manifest.update(terminal)
        # Commit the terminal manifest only after its complete summary is in place
        destination = self.path / "summary.json"
        write_json(destination, document)
        manifest["summary_sha256"] = hashlib.sha256(destination.read_bytes()).hexdigest()
        write_json(self.path / "manifest.json", manifest)
        self._manifest = manifest
        return exit_code


def create_run(
    root: Path,
    anchor: str,
    label: str,
    workload: dict[str, JSONValue],
    instruments: dict[str, JSONValue],
    observer: dict[str, JSONValue],
    *,
    kind: RunKind = "workload",
) -> RunRecord:
    """Reserve one run without overwriting earlier measurements"""
    if _COMMIT.fullmatch(anchor) is None:
        raise ValueError("run anchor must be a full Git commit hash")
    _validate_label(label)
    root = root.resolve()
    runs = (root / "src" / "runs").resolve()
    path = (runs / anchor[:7] / label).resolve()
    if not runs.is_relative_to(root) or not path.is_relative_to(runs):
        raise ValueError("run path must stay inside src/runs")
    manifest: dict[str, JSONValue] = {
        "schema_version": 1,
        "anchor": anchor,
        "label": label,
        "kind": kind,
        "status": "running",
        "started_at": _timestamp(),
        "finished_at": None,
        "exit_code": None,
        "error": None,
        "secondary_errors": [],
        "provenance": {
            "observer": deepcopy(observer),
            "source": None,
            "build": None,
            "binary": None,
        },
        "workload": deepcopy(workload),
        "instruments": deepcopy(instruments),
    }
    path.mkdir(parents=True, exist_ok=False)
    (path / "raw").mkdir()
    write_json(
        path / "summary.json",
        {key: value for key, value in manifest.items() if key != "provenance"},
    )
    write_json(path / "manifest.json", manifest)
    return RunRecord(path, manifest)
