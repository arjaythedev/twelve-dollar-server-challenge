"""Pure filesystem tests for run lifecycle and provenance"""

import hashlib
import unittest
from io import StringIO
from pathlib import Path
from unittest.mock import patch

from devtools.reports import (
    JSONValue,
    load_saved_report,
    mapping,
    read_json,
    show_saved_report,
    write_json,
)
from devtools.runs import TerminalStatus, create_run
from tests import TemporaryDirectory

ANCHOR = "a" * 40
BASE = "b" * 40
SOURCE = "c" * 64


class RunRecordTests(unittest.TestCase):
    def test_altered_source_patch_cannot_be_reported_as_a_successful_run(self) -> None:
        with TemporaryDirectory() as directory:
            run = create_run(Path(directory), ANCHOR, "altered-patch", {}, {}, {})
            run.record_source(BASE, b"verified patch\r\n", SOURCE)
            (run.path / "source.patch").write_bytes(b"formatted patch\n")
            self.assertEqual(run.finish("passed", {}, 0), 1)
            manifest = mapping(read_json(run.path / "manifest.json"))
            self.assertEqual(manifest["status"], "failed")
            self.assertEqual(manifest["exit_code"], 1)
            self.assertIn("source.patch", str(manifest["error"]))
            for destination in (run.path, run.path / "summary.json"):
                with self.assertRaisesRegex(ValueError, "source.patch"):
                    load_saved_report(destination)

    def test_native_probe_uses_the_same_record_without_http_metrics(self) -> None:
        with TemporaryDirectory() as directory:
            run = create_run(Path(directory), ANCHOR, "native-probe", {}, {}, {}, kind="probe")
            run.record_source(BASE, b"captured patch\n", SOURCE)
            run.finish(
                "passed",
                {
                    "measurement": {
                        "kind": "telemetry_hook_probe",
                        "medians": {
                            "before": {"scope_ns_per_call": 10},
                            "after": {"scope_ns_per_call": 8},
                        },
                        "reduction_percent": {"scope_ns_per_call": 20},
                    }
                },
                0,
            )
            document = load_saved_report(run.path)
            self.assertEqual(mapping(document)["kind"], "probe")
            self.assertNotIn("benchmark", mapping(document))
            with patch("sys.stdout", new_callable=StringIO) as output:
                show_saved_report(document)
            self.assertIn("native-probe (passed)", output.getvalue())
            self.assertIn("20.00%", output.getvalue())

    def test_reserves_unique_directory_and_keeps_unknown_provenance_unknown(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory)
            run = create_run(root, ANCHOR, "reference", {"vus": 5000}, {}, {"commit": BASE})
            self.assertEqual(run.path, root / "src" / "runs" / ANCHOR[:7] / "reference")
            self.assertTrue(run.raw.is_dir())
            self.assertFalse((run.path / "source.patch").exists())
            manifest = mapping(read_json(run.path / "manifest.json"))
            self.assertEqual(manifest["anchor"], ANCHOR)
            self.assertEqual(manifest["status"], "running")
            self.assertEqual(
                manifest["provenance"],
                {"observer": {"commit": BASE}, "source": None, "build": None, "binary": None},
            )
            self.assertEqual(mapping(read_json(run.path / "summary.json"))["status"], "running")
            with self.assertRaises(FileExistsError):
                create_run(root, ANCHOR, "reference", {}, {}, {})
            self.assertEqual(read_json(run.path / "manifest.json"), manifest)

    def test_label_and_anchor_validation_prevent_path_escape(self) -> None:
        invalid_labels = (
            "",
            ".",
            "..",
            "../escape",
            "nested/run",
            "nested\\run",
            "C:\\escape",
            "/escape",
            "Uppercase",
            "trailing.",
            "space name",
            "a" * 97,
            "con",
            "lpt1.txt",
        )
        with TemporaryDirectory() as directory:
            root = Path(directory)
            for label in invalid_labels:
                with self.subTest(label=label), self.assertRaises(ValueError):
                    create_run(root, ANCHOR, label, {}, {}, {})
            for anchor in ("", "abcdef0", "../" + "a" * 40, "z" * 40):
                with self.subTest(anchor=anchor), self.assertRaises(ValueError):
                    create_run(root, anchor, "valid", {}, {}, {})
            self.assertFalse((root / "src").exists())

    def test_resolved_output_path_must_stay_inside_runs(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory)
            escaped = root.parent / "escaped-run"
            original = Path.resolve

            def resolve(path: Path) -> Path:
                return escaped if path.name == "escape" else original(path)

            with (
                patch.object(Path, "resolve", resolve),
                self.assertRaisesRegex(ValueError, "inside src/runs"),
            ):
                create_run(root, ANCHOR, "escape", {}, {}, {})
            self.assertFalse((root / "src").exists())

    def test_source_patch_preserves_exact_bytes_and_full_hashes(self) -> None:
        data = b"diff --git a/file b/file\r\n+binary:\x00\xff\n"
        with TemporaryDirectory() as directory:
            run = create_run(Path(directory), ANCHOR, "candidate", {}, {}, {})
            run.record_source(BASE, data, SOURCE)
            self.assertEqual((run.path / "source.patch").read_bytes(), data)
            provenance = mapping(run.manifest["provenance"])
            self.assertEqual(
                provenance["source"],
                {
                    "base_commit": BASE,
                    "patch_sha256": hashlib.sha256(data).hexdigest(),
                    "source_sha256": SOURCE,
                    "patch": "source.patch",
                },
            )
            self.assertIsNone(provenance["build"])
            self.assertIsNone(provenance["binary"])
            with self.assertRaisesRegex(RuntimeError, "already recorded"):
                run.record_source(BASE, b"different patch", SOURCE)
            self.assertEqual((run.path / "source.patch").read_bytes(), data)

    def test_settings_and_measured_identities_remain_independent(self) -> None:
        instruments: dict[str, JSONValue] = {
            "server_telemetry": "off",
            "client_diagnostics": True,
            "perf": "kernel",
        }
        workload: dict[str, JSONValue] = {"vus": 17, "seed": 31}
        observer: dict[str, JSONValue] = {"commit": ANCHOR, "dirty": True}
        build: dict[str, JSONValue] = {"sha256": "d" * 64, "settings": {"telemetry": "off"}}
        with TemporaryDirectory() as directory:
            run = create_run(
                Path(directory), ANCHOR, "independent", workload, instruments, observer
            )
            run.record_build("server", build)
            run.record_binary("server", {"image_id": "sha256:" + "e" * 64, "sha256": "f" * 64})
            instruments["server_telemetry"] = "profile"
            workload["vus"] = 99
            observer["commit"] = BASE
            mapping(build["settings"])["telemetry"] = "profile"
            manifest = run.manifest
            self.assertEqual(
                manifest["instruments"],
                {"server_telemetry": "off", "client_diagnostics": True, "perf": "kernel"},
            )
            self.assertEqual(manifest["workload"], {"vus": 17, "seed": 31})
            provenance = mapping(manifest["provenance"])
            self.assertEqual(provenance["observer"], {"commit": ANCHOR, "dirty": True})
            self.assertIsNone(provenance["source"])
            self.assertEqual(
                mapping(provenance["build"])["server"],
                {"sha256": "d" * 64, "settings": {"telemetry": "off"}},
            )
            self.assertEqual(
                mapping(provenance["binary"])["server"],
                {"image_id": "sha256:" + "e" * 64, "sha256": "f" * 64},
            )
            manifest["status"] = "failed"
            self.assertEqual(run.manifest["status"], "running")
            run.finish("passed", {}, 0)
            saved = mapping(load_saved_report(run.path / "summary.json"))
            self.assertEqual(saved["instruments"], run.manifest["instruments"])
            self.assertEqual(saved["workload"], {"vus": 17, "seed": 31})
            self.assertEqual(mapping(saved["provenance"])["binary"], provenance["binary"])

    def test_terminal_statuses_preserve_optional_payload_and_primary_error(self) -> None:
        cases: tuple[tuple[TerminalStatus, int, str | None], ...] = (
            ("passed", 0, None),
            ("failed", 7, "workload exited 7"),
            ("interrupted", 130, "interrupted"),
        )
        with TemporaryDirectory() as directory:
            for status, exit_code, error in cases:
                with self.subTest(status=status):
                    run = create_run(Path(directory), ANCHOR, status, {}, {}, {})
                    payload: dict[str, JSONValue] = (
                        {"benchmark": {"requests": 3}} if status == "passed" else {}
                    )
                    run.finish(
                        status, payload, exit_code, error, ["collection failed", "cleanup failed"]
                    )
                    summary = mapping(read_json(run.path / "summary.json"))
                    manifest = mapping(read_json(run.path / "manifest.json"))
                    for document in (summary, manifest):
                        self.assertEqual(document["status"], status)
                        self.assertEqual(document["exit_code"], exit_code)
                        self.assertEqual(document["error"], error)
                        self.assertEqual(
                            document["secondary_errors"], ["collection failed", "cleanup failed"]
                        )
                        self.assertIsInstance(document["finished_at"], str)
                    self.assertEqual(
                        manifest["summary_sha256"],
                        hashlib.sha256((run.path / "summary.json").read_bytes()).hexdigest(),
                    )
                    self.assertEqual("benchmark" in summary, status == "passed")
                    with self.assertRaisesRegex(RuntimeError, "finished"):
                        run.finish(status, {}, exit_code)
                    with self.assertRaisesRegex(RuntimeError, "finished"):
                        run.record_build("server", {})

    def test_failed_summary_write_does_not_commit_terminal_manifest(self) -> None:
        with TemporaryDirectory() as directory:
            run = create_run(Path(directory), ANCHOR, "write-failure", {}, {}, {})
            with patch("devtools.runs.write_json", side_effect=OSError("disk full")):
                with self.assertRaisesRegex(OSError, "disk full"):
                    run.finish("failed", {}, 1, "workload failed")
            self.assertEqual(run.manifest["status"], "running")
            self.assertEqual(mapping(read_json(run.path / "manifest.json"))["status"], "running")
            run.finish("failed", {}, 1, "workload failed")
            self.assertEqual(run.manifest["status"], "failed")

    def test_terminal_manifest_is_committed_after_complete_summary(self) -> None:
        with TemporaryDirectory() as directory:
            run = create_run(Path(directory), ANCHOR, "manifest-failure", {}, {}, {})

            def fail_manifest(path: Path, value: object) -> None:
                if path.name == "manifest.json":
                    raise OSError("manifest unavailable")
                write_json(path, value)

            with patch("devtools.runs.write_json", side_effect=fail_manifest):
                with self.assertRaisesRegex(OSError, "manifest unavailable"):
                    run.finish("interrupted", {}, 130, "interrupted")
            self.assertEqual(run.manifest["status"], "running")
            self.assertEqual(mapping(read_json(run.path / "manifest.json"))["status"], "running")
            self.assertEqual(mapping(read_json(run.path / "summary.json"))["status"], "interrupted")
            run.finish("interrupted", {}, 130, "interrupted")
            self.assertEqual(run.manifest["status"], "interrupted")


if __name__ == "__main__":
    unittest.main()
