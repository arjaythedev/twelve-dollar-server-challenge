"""Local-only installer tests: fixture archives, no network or package installs.

The copied installer has system paths redirected into a temporary directory.
Toolchain commands are executable fixture scripts. Network/package operations,
host detection, and platform-specific tar/mv flags are mocked; the installer's
checksums, archive extraction, staging, validation, and rollback actually run.
Run with: python3 -m unittest discover -s submissions/go-lowlevel-janso123/tests
"""

import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest


INSTALLER = Path(__file__).resolve().parents[1] / "install.sh"
LLVM_SHA = "14d2f701eb68fb799001bdea6231048f3990690fa2f406d563555ff8f744daba"
GO_SHA = "ecbadb99091a3f46e31f5f934b068b1864eafa7995211b39eaddf76996045fe5"
LLVM_URL = "https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.3/LLVM-23.1.3-Linux-X64.tar.zst"

COMMAND_DOUBLE = r'''#!/usr/bin/env python3
import os
from pathlib import Path
import shutil
import subprocess
import sys

name = Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ["COMMAND_LOG"], "a") as log:
    log.write(name + " " + " ".join(args) + "\n")
if name == "id":
    print(os.environ.get("TEST_UID", "0"))
elif name == "uname":
    print(os.environ.get("TEST_ARCH", "x86_64") if "-m" in args else "Linux")
elif name == "apt-get":
    sys.exit(int(os.environ.get("FAIL_APT", "0")))
elif name == "curl":
    assert "--fail" in args or "-fsSL" in args, args
    assert "--retry" in args, args
    destination = Path(args[args.index("-o") + 1])
    assert destination.is_relative_to(Path(os.environ["TEST_ROOT"])), destination
    url = args[-1]
    if url == os.environ["LLVM_URL"]:
        source = os.environ["LLVM_FIXTURE"]
    elif url == "https://go.dev/dl/go1.27.2.linux-amd64.tar.gz":
        source = os.environ["GO_FIXTURE"]
    else:
        raise AssertionError("unexpected URL: " + url)
    shutil.copyfile(source, destination)
    if os.environ.get("BAD_CHECKSUM") in ("all", destination.name):
        with destination.open("ab") as stream:
            stream.write(b"corrupt")
elif name == "tar":
    # Fixture LLVM tarballs use gzip so the host needs no zstd installation.
    if any("LLVM-" in a for a in args):
        assert "--use-compress-program=zstd --long=30" in args, args
    args = [a for a in args if a not in ("--use-compress-program=zstd --long=30", "--verbatim-files-from")]
    sys.exit(subprocess.call([os.environ["REAL_TAR"], *args]))
elif name == "mv":
    args = [a for a in args if a not in ("-T", "--")]
    source, destination = map(Path, args)
    assert all(p.is_relative_to(Path(os.environ["TEST_ROOT"])) for p in (source, destination))
    if (os.environ.get("FAIL_PROMOTE") == destination.name
            and source.name == "new"):
        sys.exit(71)
    os.rename(source, destination)
else:
    raise AssertionError(name)
'''

CLANG = r'''#!/bin/sh
case "$1" in
  --version) echo "clang version ${TEST_CLANG_VERSION:-23.1.3}" ;;
  -print-resource-dir)
    bin=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
    echo "$bin/../lib/clang/23" ;;
  *)
    [ "${FAIL_SMOKE:-0}" = 0 ] || exit 72
    while [ "$#" -gt 0 ]; do
      if [ "$1" = -o ]; then
        shift
        printf '#!/bin/sh\nexit 0\n' > "$1"
        chmod +x "$1"
        exit 0
      fi
      shift
    done
    exit 73 ;;
esac
'''


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="installer-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.llvm = self.root / "opt/llvm-23.1.3"
        self.go = self.root / "usr/local/go"
        self.log = self.root / "commands.log"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        double = self.bin / "command-double"
        double.write_text(COMMAND_DOUBLE)
        double.chmod(0o755)
        for name in ("id", "uname", "apt-get", "curl", "tar", "mv"):
            (self.bin / name).symlink_to(double.name)
        self.os_release = self.root / "os-release"
        self.os_release.write_text('ID=ubuntu\nVERSION_ID="24.04"\n')
        self.llvm_archive = self.root / "llvm.tar.gz"
        self.go_archive = self.root / "go.tar.gz"
        self.write_llvm()
        self.write_go()

    def write_go(self, version="1.27.2"):
        self.write_archive(self.go_archive, {
            "go/bin/go": f"#!/bin/sh\necho 'go version go{version} linux/amd64'\n",
        })

    def write_archive(self, path, members, links=None):
        with tarfile.open(path, "w:gz") as archive:
            for name, content in members.items():
                entry = tarfile.TarInfo(name)
                data = content.encode()
                entry.mode = 0o755 if "/bin/" in name else 0o644
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
            for name, target in (links or {}).items():
                entry = tarfile.TarInfo(name)
                entry.type = tarfile.SYMTYPE
                entry.linkname = target
                archive.addfile(entry)

    def write_llvm(self, top="unexpected-archive-root", missing=None, extra=None):
        members = {
            f"{top}/bin/clang-23": CLANG,
            f"{top}/bin/llvm-ar": "#!/bin/sh\necho 'LLVM version 23.1.3'\n",
            f"{top}/bin/llvm-profdata": "#!/bin/sh\necho 'LLVM version 23.1.3'\n",
            f"{top}/bin/lld": "#!/bin/sh\necho 'LLD 23.1.3'\n",
            f"{top}/lib/clang/23/include/stddef.h": "/* fixture resource */\n",
            f"{top}/lib/libLLVM.so.23.1": "runtime library fixture\n",
            f"{top}/bin/clang-tidy": "unneeded tool\n",
            f"{top}/lib/libclang.a": "unneeded static library\n",
            f"{top}/share/doc/manual": "unneeded documentation\n",
        }
        if missing:
            del members[f"{top}/{missing}"]
        members.update(extra or {})
        links = {f"{top}/bin/{name}": target for name, target in (
            ("clang", "clang-23"), ("clang++", "clang"), ("ld.lld", "lld"),
        )}
        self.write_archive(self.llvm_archive, members, links)

    def run_installer(self, **overrides):
        # Fixture hashes replace pins in this disposable copy only. A separate
        # static test checks the production pins against saved release metadata.
        text = INSTALLER.read_text()
        for pin, archive in ((LLVM_SHA, self.llvm_archive), (GO_SHA, self.go_archive)):
            text = text.replace(pin, hashlib.sha256(archive.read_bytes()).hexdigest())
        text = text.replace("/opt", str(self.root / "opt"))
        text = text.replace("/usr/local", str(self.root / "usr/local"))
        text = text.replace("/etc/os-release", str(self.os_release))
        script = self.root / "install.sh"
        script.write_text(text)
        downloads = self.root / "downloads"
        downloads.mkdir(exist_ok=True)
        env = {
            **os.environ,
            "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
            "TMPDIR": str(downloads),
            "COMMAND_LOG": str(self.log),
            "TEST_ROOT": str(self.root),
            "LLVM_FIXTURE": str(self.llvm_archive),
            "GO_FIXTURE": str(self.go_archive),
            "LLVM_URL": LLVM_URL,
            "REAL_TAR": shutil.which("tar"),
            **overrides,
        }
        result = subprocess.run(["bash", str(script)], env=env, text=True,
                                capture_output=True, timeout=15)
        self.assertEqual(list(downloads.iterdir()), [], result.stderr)
        for parent in (self.root / "opt", self.root / "usr/local"):
            if parent.exists():
                self.assertEqual([p.name for p in parent.iterdir() if p.name.startswith(".")],
                                 [], result.stderr)
        return result

    def old_toolchains(self):
        for path in (self.llvm, self.go):
            path.mkdir(parents=True)
            (path / "old").write_text("existing toolchain")

    def assert_old_toolchains(self):
        for path in (self.llvm, self.go):
            self.assertEqual((path / "old").read_text(), "existing toolchain")
            self.assertEqual([p.name for p in path.iterdir()], ["old"])

    def test_pinned_checksums(self):
        source = INSTALLER.read_text()
        self.assertIn(LLVM_SHA, source)
        self.assertIn(GO_SHA, source)
        # The release metadata is saved locally but ignored by git. Cross-check
        # it when available without making fresh checkouts depend on it.
        metadata = INSTALLER.parents[2] / "results/goal-optimization/clang-builds/llvm-23.1.3-release.json"
        if metadata.exists():
            release = json.loads(metadata.read_text())
            asset = next(a for a in release["assets"] if a["name"] == "LLVM-23.1.3-Linux-X64.tar.zst")
            self.assertEqual(asset["digest"], "sha256:" + LLVM_SHA)
            self.assertEqual(asset["browser_download_url"], LLVM_URL)

    def test_install_discovers_root_and_extracts_only_toolchain(self):
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        for tool in ("clang", "clang-23", "clang++", "llvm-ar", "llvm-profdata", "ld.lld", "lld"):
            self.assertTrue(os.access(self.llvm / "bin" / tool, os.X_OK), tool)
        self.assertTrue((self.llvm / "lib/clang/23/include/stddef.h").is_file())
        self.assertTrue((self.llvm / "lib/libLLVM.so.23.1").is_file())
        self.assertTrue((self.go / "bin/go").is_file())
        for name in ("bin/clang-tidy", "lib/libclang.a", "share/doc/manual"):
            self.assertFalse((self.llvm / name).exists(), name)

    def test_repeated_install_replaces_verified_toolchains(self):
        for _ in range(2):
            result = self.run_installer()
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.go / "bin/go").exists())
        self.assertTrue((self.llvm / "bin/clang").exists())

    def test_leading_dot_archive_root_is_supported(self):
        self.write_llvm(top="./different-root")
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.llvm / "bin/clang").exists())

    def test_root_and_architecture_are_checked_before_side_effects(self):
        for overrides in ({"TEST_UID": "1000"}, {"TEST_ARCH": "aarch64"}):
            with self.subTest(overrides=overrides):
                self.log.unlink(missing_ok=True)
                result = self.run_installer(**overrides)
                self.assertNotEqual(result.returncode, 0)
                log = self.log.read_text()
                self.assertNotIn("apt-get", log)
                self.assertNotIn("curl", log)

    def test_unsupported_ubuntu_is_rejected_before_side_effects(self):
        self.os_release.write_text('ID=ubuntu\nVERSION_ID="22.04"\n')
        result = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("apt-get", self.log.read_text())

    def test_failed_checksum_never_extracts_or_replaces(self):
        self.old_toolchains()
        result = self.run_installer(BAD_CHECKSUM="all")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("computed checksum did NOT match", result.stderr)
        self.assertNotIn("tar ", self.log.read_text())
        self.assert_old_toolchains()

    def test_go_checksum_failure_preserves_both_toolchains(self):
        self.old_toolchains()
        result = self.run_installer(BAD_CHECKSUM="go1.27.2.linux-amd64.tar.gz")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("computed checksum did NOT match", result.stderr)
        self.assertNotIn("tar ", self.log.read_text())
        self.assert_old_toolchains()

    def test_multiple_archive_roots_are_rejected(self):
        self.old_toolchains()
        self.write_llvm(extra={"second-root/bin/tool": "untrusted layout"})
        result = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected exactly one LLVM archive root", result.stderr)
        self.assert_old_toolchains()

    def test_archive_path_traversal_is_rejected(self):
        self.old_toolchains()
        self.write_llvm(extra={"unexpected-archive-root/../escape": "unsafe path"})
        result = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsafe LLVM archive path", result.stderr)
        self.assert_old_toolchains()

    def test_missing_tool_preserves_both_toolchains(self):
        self.old_toolchains()
        self.write_llvm(missing="bin/llvm-profdata")
        result = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("staged LLVM version check failed", result.stderr)
        self.assert_old_toolchains()

    def test_failed_staged_verification_preserves_both_toolchains(self):
        self.old_toolchains()
        for overrides in ({"TEST_CLANG_VERSION": "23.1.4"},
                          {"TEST_CLANG_VERSION": "23.1.30"}, {"FAIL_SMOKE": "1"}):
            with self.subTest(overrides=overrides):
                result = self.run_installer(**overrides)
                self.assertNotEqual(result.returncode, 0)
                if "TEST_CLANG_VERSION" in overrides:
                    self.assertIn("staged LLVM version check failed", result.stderr)
                else:
                    self.assertEqual(result.returncode, 72, result.stderr)
                self.assert_old_toolchains()

    def test_missing_clang_resources_preserves_both_toolchains(self):
        self.old_toolchains()
        self.write_llvm(missing="lib/clang/23/include/stddef.h")
        result = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("staged Clang resources are missing", result.stderr)
        self.assert_old_toolchains()

    def test_wrong_go_version_preserves_both_toolchains(self):
        self.old_toolchains()
        self.write_go(version="1.27.3")
        result = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("staged Go version check failed", result.stderr)
        self.assert_old_toolchains()

    def test_failed_second_promotion_rolls_back_both_toolchains(self):
        self.old_toolchains()
        result = self.run_installer(FAIL_PROMOTE="go")
        self.assertEqual(result.returncode, 71, result.stderr)
        self.assert_old_toolchains()

    def test_failed_first_install_removes_promoted_toolchain(self):
        result = self.run_installer(FAIL_PROMOTE="go")
        self.assertEqual(result.returncode, 71, result.stderr)
        self.assertFalse(self.llvm.exists())
        self.assertFalse(self.go.exists())


if __name__ == "__main__":
    unittest.main()
