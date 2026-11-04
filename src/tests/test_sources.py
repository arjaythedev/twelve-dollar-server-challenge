"""Source identity and exact patch reconstruction in disposable Git repositories."""

import hashlib
import os
import shutil
import subprocess
import unittest
from dataclasses import FrozenInstanceError
from pathlib import Path
from unittest.mock import patch

from devtools import sources
from tests import TemporaryDirectory


def git(root: Path, *arguments: str, data: bytes | None = None) -> bytes:
    executable = shutil.which("git")
    if executable is None:
        raise RuntimeError("Git is required for source tests")
    return subprocess.run(  # noqa: S603 - disposable local Git repository, no shell
        [executable, "-C", str(root), *arguments],
        input=data,
        capture_output=True,
        check=True,
    ).stdout


class SourceTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = TemporaryDirectory(prefix="twelve-dollar-source-test-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.root = self.directory / "checkout"
        self.root.mkdir()
        git(self.root, "init", "--quiet")
        git(self.root, "config", "user.name", "Source Test")
        git(self.root, "config", "user.email", "source-test@example.invalid")
        git(self.root, "config", "core.autocrlf", "false")
        self.write("src/Dockerfile", b"FROM scratch\nCOPY server /source\n")
        self.write("src/CMakeLists.txt", b"project(example)\n")
        self.write("src/server/main.cpp", b"base\n")
        self.write("src/server/deleted.hpp", b"delete me\n")
        self.write("src/server/data.bin", b"\x00old\xff\n")
        self.write("src/build.sh", b"#!/bin/sh\ntrue\n")
        self.write("src/benchmarks/results.json", b"[]\n")
        self.write("src/tests/native.cpp", b"native test\n")
        self.write("src/cmake/dependencies.cmake", b"pinned dependency\n")
        self.write("src/uv.lock", b"version = 1\n")
        self.write("src/docker/seccomp.json", b'{"defaultAction":"SCMP_ACT_ALLOW"}\n')
        self.write("bench/load.js", b"workload\n")
        self.write("schema.sql", b"CREATE TABLE example(id INTEGER);\n")
        self.write("README.md", b"outside source identity\n")
        git(self.root, "add", "--all")
        git(self.root, "commit", "--quiet", "--message=base")
        self.base = git(self.root, "rev-parse", "HEAD").decode("ascii").strip()

    def write(self, name: str, data: bytes) -> Path:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def assert_reconstructs(self, snapshot: sources.SourceSnapshot) -> None:
        reconstruction = self.directory / "reconstruction"
        git(
            self.directory, "clone", "--quiet", "--no-checkout", str(self.root), str(reconstruction)
        )
        git(reconstruction, "read-tree", snapshot.base_commit)
        if snapshot.patch:
            git(reconstruction, "apply", "--cached", "--binary", data=snapshot.patch)
        for source in snapshot.files:
            self.assertEqual(git(reconstruction, "show", ":" + source.path), source.data)
            entry = git(reconstruction, "ls-files", "--stage", "--", source.path)
            self.assertEqual(entry.split(maxsplit=1)[0].decode("ascii"), source.mode)
        self.assertEqual(git(reconstruction, "show", ":README.md"), b"outside source identity\n")
        self.assertNotIn(b"src/server/deleted.hpp", git(reconstruction, "ls-files"))

    def test_staged_unstaged_new_deleted_and_binary_inputs_reconstruct(self) -> None:
        self.write("src/server/main.cpp", b"staged\n")
        git(self.root, "add", "src/server/main.cpp")
        self.write("src/server/main.cpp", b"staged\nunstaged\n")
        self.write("src/server/new.hpp", b"new header\n")
        self.write("src/server/data.bin", b"\x00new\xfe\x01\n")
        self.write("src/server/new.bin", b"\x00new input\xff\n")
        (self.root / "src/server/deleted.hpp").unlink()
        self.write("README.md", b"unrelated dirty documentation\n")
        index = (self.root / ".git/index").read_bytes()
        status = git(self.root, "status", "--porcelain=v1", "-z")
        refs = git(self.root, "show-ref")
        objects = sorted(
            path.relative_to(self.root) for path in (self.root / ".git/objects").rglob("*")
        )

        with sources.capture(self.root) as snapshot:
            self.assertEqual(snapshot.base_commit, self.base)
            self.assertEqual(snapshot.context, snapshot.root / "src")
            self.assertEqual(snapshot.patch_sha256, hashlib.sha256(snapshot.patch).hexdigest())
            self.assertEqual(len(snapshot.source_sha256), 64)
            self.assertIn(b"GIT binary patch", snapshot.patch)
            self.assertIn(b"new file mode", snapshot.patch)
            self.assertIn(b"deleted file mode", snapshot.patch)
            self.assertIn(b"staged\n+unstaged", snapshot.patch)
            self.assertNotIn(b"README.md", snapshot.patch)
            self.assert_reconstructs(snapshot)
            destination = self.directory / "source.patch"
            snapshot.write_patch(destination)
            self.assertEqual(destination.read_bytes(), snapshot.patch)

        self.assertFalse(snapshot.context.exists())
        self.assertEqual((self.root / ".git/index").read_bytes(), index)
        self.assertEqual(git(self.root, "status", "--porcelain=v1", "-z"), status)
        self.assertEqual(git(self.root, "show-ref"), refs)
        self.assertEqual(
            sorted(path.relative_to(self.root) for path in (self.root / ".git/objects").rglob("*")),
            objects,
        )

    def test_outputs_caches_environment_and_upstream_are_excluded(self) -> None:
        excluded = {
            "src/benchmarks/results.json": b"changed result\n",
            "src/benchmarks/new-results.json": b"new result\n",
            "src/profiles/report.json": b"profile result\n",
            "src/runs/base/run/source.patch": b"previous patch\n",
            "src/runs/base/run/summary.json": b"run result\n",
            "src/.build/server/generated.cpp": b"cache\n",
            "src/.venv/lib/generated.py": b"environment\n",
            "src/server/__pycache__/cache.pyc": b"cache\n",
            "src/server/.env": b"environment\n",
            "src/.env.local": b"environment\n",
            "seed/feed.db": b"seed\n",
            "submissions/other/server.cpp": b"upstream\n",
            "results/report.json": b"result\n",
        }
        for name, data in excluded.items():
            self.write(name, data)
        self.write("src/benchmarks/new_probe.cpp", b"new native probe\n")
        self.write("src/tests/new_test.cpp", b"new native test\n")
        self.write("src/devtools/new_tool.py", b"new run tool\n")
        self.write(".gitignore", b"src/server/ignored.hpp\n")
        self.write("src/server/ignored.hpp", b"ignored but consumed by the build\n")
        with sources.capture(self.root) as snapshot:
            names = {source.path for source in snapshot.files}
            self.assertFalse(names.intersection(excluded))
            for name in (
                "src/benchmarks/new_probe.cpp",
                "src/tests/new_test.cpp",
                "src/devtools/new_tool.py",
                "src/server/ignored.hpp",
                "src/cmake/dependencies.cmake",
                "src/uv.lock",
                "src/docker/seccomp.json",
                "bench/load.js",
                "schema.sql",
            ):
                self.assertIn(name, names)
                self.assertEqual(
                    (snapshot.root / name).read_bytes(), (self.root / name).read_bytes()
                )
            for name in excluded:
                self.assertFalse((snapshot.root / name).exists())

    def test_raw_crlf_is_not_normalized_by_git_attributes(self) -> None:
        self.write("src/.gitattributes", b"*.cpp text eol=lf\n")
        git(self.root, "add", "src/.gitattributes")
        git(self.root, "commit", "--quiet", "--message=attributes")
        git(self.root, "config", "core.autocrlf", "true")
        self.write("src/server/main.cpp", b"staged\r\n")
        git(self.root, "add", "src/server/main.cpp")
        self.write("src/server/main.cpp", b"staged\r\nunstaged\r\n")
        (self.root / "src/server/deleted.hpp").unlink()
        with sources.capture(self.root) as snapshot:
            self.assertEqual(
                (snapshot.context / "server/main.cpp").read_bytes(), b"staged\r\nunstaged\r\n"
            )
            self.assertIn(b"+staged\r\n+unstaged\r\n", snapshot.patch)
            self.assert_reconstructs(snapshot)

    def test_changes_after_freeze_cannot_change_patch_or_build_context(self) -> None:
        self.write("src/server/main.cpp", b"measured bytes\n")
        real_patch = sources._patch  # pyright: ignore[reportPrivateUsage]

        def mutate_then_patch(
            root: Path,
            temporary: Path,
            base_commit: str,
            base_modes: dict[str, str],
            files: tuple[sources.SourceFile, ...],
        ) -> bytes:
            self.write("src/server/main.cpp", b"edited during patch generation\n")
            self.write("src/server/later.hpp", b"created after capture\n")
            return real_patch(root, temporary, base_commit, base_modes, files)

        with (
            patch.object(sources, "_patch", side_effect=mutate_then_patch),
            sources.capture(self.root) as snapshot,
        ):
            self.assertEqual(
                (snapshot.context / "server/main.cpp").read_bytes(), b"measured bytes\n"
            )
            self.assertIn(b"+measured bytes", snapshot.patch)
            self.assertNotIn(b"edited during", snapshot.patch)
            self.assertNotIn(b"later.hpp", snapshot.patch)
            self.assertFalse((snapshot.context / "server/later.hpp").exists())
            with self.assertRaises(FrozenInstanceError):
                setattr(snapshot, "base_commit", "changed")  # noqa: B010 - test runtime immutability
            original_hash = snapshot.source_sha256
        with sources.capture(self.root) as following:
            self.assertNotEqual(following.source_sha256, original_hash)

    def test_hashes_are_stable_and_executable_modes_are_preserved(self) -> None:
        script = self.root / "src/build.sh"
        if os.name != "nt":
            script.chmod(0o755)
        git(self.root, "update-index", "--chmod=+x", "src/build.sh")
        with sources.capture(self.root) as first:
            hashes = (first.patch_sha256, first.source_sha256)
            source = next(source for source in first.files if source.path == "src/build.sh")
            self.assertEqual(source.mode, "100755")
            self.assertIn(b"old mode 100644\nnew mode 100755", first.patch)
            if os.name != "nt":
                self.assertTrue((first.context / "build.sh").stat().st_mode & 0o111)
        with sources.capture(self.root) as second:
            self.assertEqual((second.patch_sha256, second.source_sha256), hashes)

    def test_unchanged_inputs_have_a_verified_empty_patch(self) -> None:
        with sources.capture(self.root) as snapshot:
            self.assertEqual(snapshot.patch, b"")
            self.assertEqual(snapshot.patch_sha256, hashlib.sha256(b"").hexdigest())
            self.assertEqual(snapshot.base_commit, self.base)


if __name__ == "__main__":
    unittest.main()
