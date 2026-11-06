"""Build contracts and optional local Go profile checks; no Linux compilation."""
import contextlib
import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = Path(__file__).resolve().parent / "fixtures"
LOCAL_GO = shutil.which("go")
LOCAL_ENV = dict(os.environ)
REAL_SUBPROCESS_RUN = subprocess.run
sys.dont_write_bytecode = True


def load_builder():
    spec = importlib.util.spec_from_file_location("submission_build", ROOT / "build.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CLI(unittest.TestCase):
    def test_help_needs_no_toolchain(self):
        result = subprocess.run([sys.executable, str(ROOT / "build.py"), "--help"],
                                capture_output=True, text=True,
                                env={"PATH": os.environ["PATH"]})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--pgo", result.stdout)
        self.assertIn("--thinlto", result.stdout)
        self.assertIn("--no-pgo", result.stdout)
        self.assertIn("--no-thinlto", result.stdout)
        self.assertIn("--no-go-pgo", result.stdout)

    def test_go_pgo_make_targets_select_bundled_profile_explicitly(self):
        builder = load_builder()
        for target, tests in (("go-pgo", False), ("test-go-pgo", True)):
            with self.subTest(target=target):
                result = subprocess.run(["make", "--no-print-directory", "-n", target],
                                        cwd=ROOT, capture_output=True, text=True,
                                        env={"PATH": os.environ["PATH"]})
                self.assertEqual(result.returncode, 0, result.stderr)
                args = builder.arguments(shlex.split(result.stdout.strip())[1:])
                self.assertEqual((args.pgo, args.thinlto, args.test), (True, True, tests))
                self.assertEqual(args.go_pgo, "profiles/go.pprof")
                self.assertEqual(args.go_pgo_sha256, hashlib.sha256((ROOT / args.go_pgo).read_bytes()).hexdigest())

    def test_make_targets_keep_independent_optimization_choices(self):
        builder = load_builder()
        for target, expected in ((None, (True, True)), ("all", (True, True)),
                                 ("nonpgo", (False, True)), ("nolto", (True, False)),
                                 ("baseline", (False, True)), ("pgo", (True, True)),
                                 ("thinlto", (True, True)), ("no-go-pgo", (True, True)),
                                 ("test-no-go-pgo", (True, True))):
            with self.subTest(target=target):
                result = subprocess.run(["make", "--no-print-directory", "-n", *([target] if target else [])],
                                        cwd=ROOT, capture_output=True, text=True,
                                        env={"PATH": os.environ["PATH"]})
                self.assertEqual(result.returncode, 0, result.stderr)
                command = shlex.split(result.stdout.strip())
                self.assertEqual(command[0], "./build.sh")
                args = builder.arguments(command[1:])
                self.assertEqual((args.pgo, args.thinlto), expected)
                self.assertEqual(args.go_pgo is not None,
                                 target not in ("baseline", "no-go-pgo", "test-no-go-pgo"))
                self.assertEqual(args.test, target == "test-no-go-pgo")


class Contracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.b = load_builder()

    def test_default_enables_bundled_go_sqlite_pgo_and_thinlto_without_tests(self):
        args = self.b.arguments([])
        self.assertTrue(args.pgo)
        self.assertTrue(args.thinlto)
        self.assertFalse(args.test)
        self.assertFalse(args.baseline)
        self.assertEqual(args.go_pgo, str(ROOT / "profiles/go.pprof"))
        self.assertEqual(args.go_pgo_sha256,
                         "cffbedaaa8f7208e6cce79844a858e8ae2f08a3a14079cdc345f0968e63eb6ff")

    def test_pgo_and_thinlto_flags_are_independent(self):
        for options, pgo, thinlto in (
                ([], True, True), (["--pgo", "--thinlto"], True, True),
                (["--no-pgo"], False, True), (["--no-pgo", "--thinlto"], False, True),
                (["--no-thinlto"], True, False), (["--pgo", "--no-thinlto"], True, False),
                (["--no-pgo", "--no-thinlto"], False, False)):
            with self.subTest(options=options):
                args = self.b.arguments(options + ["--test"])
                self.assertEqual((args.pgo, args.thinlto, args.test), (pgo, thinlto, True))

    def test_baseline_disables_pgo_and_preserves_thinlto_choice(self):
        for options, thinlto in (([], True), (["--no-pgo"], True),
                                 (["--thinlto"], True), (["--no-thinlto"], False)):
            with self.subTest(options=options):
                args = self.b.arguments(["--baseline", *options])
                self.assertEqual((args.pgo, args.thinlto, args.baseline), (False, thinlto, True))
                self.assertIsNone(args.go_pgo)
                self.assertIsNone(args.go_pgo_sha256)
        for options in (["--baseline", "--pgo"], ["--pgo", "--baseline"]):
            with self.subTest(options=options), contextlib.redirect_stderr(io.StringIO()) as error:
                with self.assertRaises(SystemExit):
                    self.b.arguments(options)
                self.assertIn("--baseline cannot be combined with --pgo", error.getvalue())

    def test_invalid_modes_and_ambient_flags_are_rejected(self):
        for args in (["generate"], ["--pgo=auto"], ["--profile", "other"],
                     ["--pgo", "--baseline"], ["--pgo", "--no-pgo"],
                     ["--no-pgo", "--pgo"], ["--thinlto", "--no-thinlto"],
                     ["--no-thinlto", "--thinlto"], ["--thinlto=full"], ["--wat"]):
            with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    self.b.arguments(args)
        for name, value in (("CGO_CFLAGS", "-O3"), ("GOFLAGS", "-tags=sqlite_llvm_pgo"),
                            ("LLVM_PROFILE_FILE", "stale.profraw"), ("GO_PGO", "auto"),
                            ("GOARCH", "arm64"), ("GOOS", "darwin")):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                self.b.validate_environment({name: value})

    def fixture(self, directory):
        raw = directory / "sqlite.profraw"
        raw.write_bytes(b"fixture; LLVM itself validates instrumentation data on Linux")
        sources = {"sqlite3-binding.c": "a" * 64}
        flags = ["--no-default-config", "-O2", "-march=x86-64-v3", "-DSQLITE_THREADSAFE=1"]
        manifest = {
            "schema": 1, "llvm_version": "23.1.3", "target": "linux/amd64", "goamd64": "v3",
            "source_filename": "/training/sqlite3.c",
            "sqlite_module": {"path": "github.com/mattn/go-sqlite3", "version": "v1.14.52"},
            "raw": {"file": "sqlite.profraw", "sha256": hashlib.sha256(raw.read_bytes()).hexdigest(),
                    "size": raw.stat().st_size},
            "sqlite_sources": sources, "sqlite_flags": flags,
            "generate_flags": ["-fprofile-generate", "-fprofile-update=atomic"],
            "go": {"pgo": "off", "tags": "cgo_sqlite,libsqlite3,sqlite_serialize"},
            "provenance": {"training_id": "measured-training", "contract_sha256": "b" * 64,
                           "binary_sha256": "c" * 64},
        }
        return raw, sources, flags, manifest

    def test_profile_contract_rejects_stale_flags_sources_and_provenance(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            raw, sources, flags, manifest = self.fixture(directory)
            path = directory / "manifest.json"
            path.write_text(json.dumps(manifest))
            self.b.validate_profile(directory, sources, flags)
            changes = [("llvm_version", "22.1.0"), ("target", "darwin/arm64"), ("goamd64", "v1"),
                       ("schema", 2), ("sqlite_flags", flags + ["-flto"]),
                       ("sqlite_sources", {"sqlite3-binding.c": "d" * 64}),
                       ("generate_flags", ["-fprofile-instr-generate"]),
                       ("go", {"pgo": "auto", "tags": "cgo_sqlite"}), ("provenance", {})]
            for key, value in changes:
                with self.subTest(key=key):
                    path.write_text(json.dumps(manifest | {key: value}))
                    with self.assertRaisesRegex(ValueError, key):
                        self.b.validate_profile(directory, sources, flags)
            for key, value in (("file", "../outside.profraw"), ("size", 0), ("sha256", "d" * 64)):
                with self.subTest(raw=key):
                    path.write_text(json.dumps(manifest | {"raw": manifest["raw"] | {key: value}}))
                    with self.assertRaisesRegex(ValueError, "raw"):
                        self.b.validate_profile(directory, sources, flags)
            path.write_text(json.dumps(manifest))
            raw.write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "raw"):
                self.b.validate_profile(directory, sources, flags)
            raw.unlink()
            with self.assertRaisesRegex(ValueError, "raw"):
                self.b.validate_profile(directory, sources, flags)
            raw.symlink_to(path)
            with self.assertRaisesRegex(ValueError, "raw"):
                self.b.validate_profile(directory, sources, flags)
            raw.unlink()
            raw.touch()
            with self.assertRaisesRegex(ValueError, "raw"):
                self.b.validate_profile(directory, sources, flags)
            path.write_text("{broken")
            with self.assertRaises(ValueError):
                self.b.validate_profile(directory, sources, flags)

    def test_prepared_headers_and_source_hash_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            module, work = Path(tmp) / "module", Path(tmp) / "work"
            module.mkdir()
            work.mkdir()
            files = {"sqlite3-binding.c": b"int sqlite3_fixture;\n",
                     "sqlite3-binding.h": b"#ifndef USE_LIBSQLITE3\nheader\n#else // USE_LIBSQLITE3\nwrong\n#endif\n",
                     "sqlite3ext.h": b"#ifndef USE_LIBSQLITE3\nextension\n#else // USE_LIBSQLITE3\nwrong\n#endif\n",
                     "sqlite3.go": b"#cgo CFLAGS: -std=gnu99\n#cgo CFLAGS: -DSQLITE_THREADSAFE=1\n",
                     "sqlite3_other.go": b"#cgo CFLAGS: -I.\n"}
            pins = {}
            for name, data in files.items():
                (module / name).write_bytes(data)
                pins[name] = hashlib.sha256(data).hexdigest()
            self.b.prepare_sqlite(module, work, pins)
            self.assertEqual((work / "sqlite3.h").read_bytes(), b"header\n")
            self.assertEqual((work / "sqlite3ext.h").read_bytes(), b"extension\n")
            (module / "sqlite3.go").write_bytes(b"changed CFLAGS")
            with self.assertRaisesRegex(ValueError, "sqlite3.go"):
                self.b.prepare_sqlite(module, work, pins)

    def test_invalid_request_removes_old_server(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(self.b, "ROOT", Path(tmp)):
            binary = Path(tmp) / "bin/server"
            binary.parent.mkdir()
            binary.write_bytes(b"old output must not look like this failed build")
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                self.b.main(["--pgo", "--baseline"])
            self.assertFalse(binary.exists())

    def test_profile_relocation_preserves_counters_and_rejects_wrong_source(self):
        shown = ("  /training/sqlite3.c;internal:\n    Hash: 0x1234\n"
                 "    Counters: 2\n    Block counts: [42, 0]\n"
                 "  sqlite3_step:\n    Hash: 0x5678\n    Counters: 1\n    Block counts: [7]\n")
        records = self.b.profile_records(shown)
        pairs, relocated = self.b.relocated_records(records, "/training/sqlite3.c", "/checkout/build/sqlite3.c")
        self.assertEqual(pairs, {"/training/sqlite3.c;internal": "/checkout/build/sqlite3.c;internal"})
        self.assertEqual(relocated[("/checkout/build/sqlite3.c;internal", "0x1234")], [42, 0])
        self.assertEqual(relocated[("sqlite3_step", "0x5678")], [7])
        with self.assertRaisesRegex(ValueError, "source"):
            self.b.relocated_records(records, "/wrong/sqlite3.c", "/checkout/sqlite3.c")
        with self.assertRaisesRegex(ValueError, "counter count"):
            self.b.profile_records(shown.replace("Counters: 2", "Counters: 3"))
        with self.assertRaisesRegex(ValueError, "record format"):
            self.b.profile_records(shown + "  unparsed:\n")

    def test_pgo_never_falls_back_on_unsupported_cpu(self):
        for pgo in (False, True):
            with self.subTest(pgo=pgo):
                if pgo:
                    with self.assertRaisesRegex(ValueError, "v3"):
                        self.b.select_cpu(False, pgo=True, baseline=False)
                else:
                    self.assertEqual(self.b.select_cpu(False, pgo=False, baseline=False), "v1")
        self.assertEqual(self.b.select_cpu(True, pgo=False, baseline=False), "v3")
        self.assertEqual(self.b.select_cpu(True, pgo=False, baseline=True), "v1")


    def test_go_pgo_flags_require_an_explicit_pair_and_valid_digest(self):
        for options in (["--go-pgo", "cpu.pprof"], ["--go-pgo-sha256", "a" * 64],
                        ["--go-pgo", "cpu.pprof", "--go-pgo-sha256", "bad"],
                        ["--go-pgo", "cpu.pprof", "--go-pgo-sha256", "g" * 64]):
            with self.subTest(options=options), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    self.b.arguments(options)
        args = self.b.arguments(["--go-pgo", "cpu.pprof", "--go-pgo-sha256", "a" * 64])
        self.assertEqual((args.go_pgo, args.go_pgo_sha256), ("cpu.pprof", "a" * 64))

    def test_no_go_pgo_is_independent_of_sqlite_and_thinlto(self):
        for options, pgo, thinlto in (([], True, True), (["--no-pgo"], False, True),
                                     (["--no-thinlto"], True, False),
                                     (["--no-pgo", "--no-thinlto"], False, False),
                                     (["--baseline"], False, True)):
            with self.subTest(options=options):
                args = self.b.arguments([*options, "--no-go-pgo"])
                self.assertEqual((args.pgo, args.thinlto), (pgo, thinlto))
                self.assertIsNone(args.go_pgo)
                self.assertIsNone(args.go_pgo_sha256)

    def test_go_pgo_remains_default_across_sqlite_and_thinlto_choices(self):
        for options in ([], ["--pgo"], ["--no-pgo"], ["--no-thinlto"],
                        ["--no-pgo", "--no-thinlto"]):
            with self.subTest(options=options):
                args = self.b.arguments(options)
                self.assertEqual(args.go_pgo, str(ROOT / "profiles/go.pprof"))
                self.assertEqual(args.go_pgo_sha256,
                                 "cffbedaaa8f7208e6cce79844a858e8ae2f08a3a14079cdc345f0968e63eb6ff")

    def test_go_pgo_opt_out_and_baseline_reject_custom_profile_options(self):
        for conflict in ("--no-go-pgo", "--baseline"):
            for custom in (["--go-pgo", "cpu.pprof"], ["--go-pgo-sha256", "a" * 64],
                           ["--go-pgo", "cpu.pprof", "--go-pgo-sha256", "a" * 64],
                           ["--go-pgo=cpu.pprof", "--go-pgo-sha256=" + "a" * 64]):
                for options in ([conflict, *custom], [*custom, conflict]):
                    with self.subTest(options=options), contextlib.redirect_stderr(io.StringIO()):
                        with self.assertRaises(SystemExit):
                            self.b.arguments(options)


class CPUProfiles(unittest.TestCase):
    # Small, independently encoded profile.proto fields for wire-format edges.
    CPU = b"\x0a\x04\x08\x01\x10\x02"
    STRINGS = b"\x32\x00\x32\x03cpu\x32\x0bnanoseconds"
    ONE = b"\x12\x02\x10\x01"

    def setUp(self):
        self.b = load_builder()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "cpu.pprof"

    def validate(self, data):
        self.path.write_bytes(data)
        self.b.validate_go_cpu_samples(self.path)

    def test_bundled_profile_matches_provenance_and_has_positive_cpu(self):
        provenance = json.loads((ROOT / "profiles/go-provenance.json").read_text())
        self.b.validate_go_profile(ROOT / "profiles/go.pprof", provenance["profile_sha256"])
        self.b.validate_go_cpu_samples(ROOT / "profiles/go.pprof")

    def test_recorded_cpu_fixtures_require_exact_units_and_positive_weight(self):
        self.b.validate_go_cpu_samples(FIXTURES / "positive.pprof")
        for name, message in (("zero", "positive CPU weight"),
                              ("wrong-unit", "cpu/nanoseconds")):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, message):
                self.b.validate_go_cpu_samples(FIXTURES / (name + ".pprof"))

    def test_mixed_profiles_require_positive_first_compiler_eligible_column(self):
        for name in ("count-first-positive", "cpu-first-positive"):
            with self.subTest(name=name):
                self.b.validate_go_cpu_samples(FIXTURES / (name + ".pprof"))
        with self.assertRaisesRegex(ValueError, "positive compiler-selected weight.*samples/count"):
            self.b.validate_go_cpu_samples(FIXTURES / "count-first-zero.pprof")
        for name in ("cpu-first-zero", "count-first-zero-cpu"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "positive CPU weight"):
                self.b.validate_go_cpu_samples(FIXTURES / (name + ".pprof"))

    @unittest.skipUnless(LOCAL_GO, "compiler selection checks require installed Go preprofile")
    def test_real_preprofile_uses_first_eligible_column_in_both_orders(self):
        # preprofile is the actual compiler PGO parser, not pprof's display
        # column selector. Recorded stacks give nonempty edges iff usable.
        for name, total in (("count-first-zero", 0), ("count-first-positive", 135),
                            ("cpu-first-zero", 0), ("cpu-first-positive", 1350000000),
                            ("count-first-zero-cpu", 135)):
            with self.subTest(name=name):
                result = REAL_SUBPROCESS_RUN(
                    [LOCAL_GO, "tool", "preprofile", "-i", str(FIXTURES / (name + ".pprof"))],
                    capture_output=True, text=True,
                    env=dict(LOCAL_ENV, GOTOOLCHAIN="local", GOPROXY="off", GOSUMDB="off",
                             GOENV="off", GOWORK="off", GOTELEMETRY="off"))
                self.assertEqual(result.returncode, 0, result.stderr)
                lines = result.stdout.splitlines()
                self.assertEqual(lines[0], "GO PREPROFILE V1")
                self.assertEqual((len(lines) - 1) % 3, 0)
                self.assertEqual((len(lines) - 1) // 3, 0 if total == 0 else 4)
                self.assertEqual(sum(int(line.split()[1]) for line in lines[3::3]), total)

    def test_accepts_one_nanosecond_packed_unpacked_gzip_and_extra_sample_types(self):
        count_type = b"\x0a\x04\x08\x03\x10\x04"
        strings = self.STRINGS + b"\x32\x07samples\x32\x05count"
        for sample in (b"\x12\x04\x10\x01\x10\x01",  # unpacked: count=1, cpu=1
                       b"\x12\x04\x12\x02\x01\x01"):  # packed
            # String table before types; unrelated period/default/extension data
            # must not decide CPU validity. Zero samples alongside one are fine.
            data = strings + count_type + self.CPU + sample + b"\x12\x04\x10\x00\x10\x00"
            data += b"\x70\x03\xf8\x07\x01\xf9\x07" + b"\x00" * 8
            data += b"\xfa\x07\x01x\xfd\x07" + b"\x00" * 4
            for compressed in (False, True):
                with self.subTest(sample=sample, compressed=compressed):
                    self.validate(gzip.compress(data) if compressed else data)
        # Counts need not be present, and a CPU-only profile may be uncompressed.
        self.validate(self.CPU + self.ONE + self.STRINGS)

    def test_selected_count_uses_exact_type_unit_and_signed_total(self):
        strings = self.STRINGS + b"\x32\x07samples\x32\x05count"
        count = b"\x0a\x04\x08\x03\x10\x04"
        negative = b"\x12\x0d\x10" + b"\xff" * 9 + b"\x01\x10\x01"
        positive = b"\x12\x04\x10\x01\x10\x01"
        for samples in (negative, negative + positive):
            with self.subTest(samples=samples), self.assertRaisesRegex(
                    ValueError, "positive compiler-selected weight.*samples/count"):
                self.validate(count + self.CPU + samples + strings)
        self.validate(count + self.CPU + negative + positive * 2 + strings)
        # A leading ineligible column with zero weight must be skipped, even
        # if its name or unit separately resembles an eligible column.
        zero_then_one = b"\x12\x04\x10\x00\x10\x01"
        for ineligible in (b"\x0a\x04\x08\x03\x10\x02",  # samples/nanoseconds
                           b"\x0a\x04\x08\x01\x10\x04"):  # cpu/count
            # The cpu/count case retains the previous first-cpu unit guard.
            if ineligible.endswith(b"\x10\x04"):
                with self.assertRaisesRegex(ValueError, "cpu/nanoseconds"):
                    self.validate(ineligible + self.CPU + zero_then_one + strings)
            else:
                self.validate(ineligible + self.CPU + zero_then_one + strings)

    def test_nonpositive_signed_total_is_rejected(self):
        negative = b"\x12\x0b\x10" + b"\xff" * 9 + b"\x01"  # int64(-1)
        for samples in (b"", b"\x12\x02\x10\x00", negative, self.ONE + negative):
            with self.subTest(samples=samples), self.assertRaisesRegex(ValueError, "positive CPU weight"):
                self.validate(self.CPU + samples + self.STRINGS)

    def test_malformed_profiles_fail_closed(self):
        good = self.CPU + self.ONE + self.STRINGS
        for data in (b"\x80", b"\xff" * 10, b"\x00", b"\x0b", b"\x0a\x10x",
                     b"\x09x", b"\x0dx", b"\x08\x01", b"\x1f\x8b",
                     good + b"\x80", self.CPU + self.ONE,
                     b"\x0a\x04\x08\x09\x10\x02" + self.ONE + self.STRINGS,
                     b"\x0a\x02\x0a\x00" + self.ONE + self.STRINGS,
                     self.CPU + b"\x12\x00" + self.STRINGS,
                     self.CPU + b"\x12\x05\x15abcd" + self.STRINGS,
                     self.CPU + b"\x12\x03\x12\x01\x80" + self.STRINGS):
            with self.subTest(data=data), self.assertRaisesRegex(ValueError, "Go PGO profile"):
                self.validate(data)



class Pipeline(unittest.TestCase):
    """Replace unavailable Linux executables at the subprocess boundary only."""
    def setUp(self):
        self.b = load_builder()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.module = self.root / "module"
        self.module.mkdir()
        sources = {
            "sqlite3-binding.c": b"int sqlite3_fixture;\n",
            "sqlite3-binding.h": b"#ifndef USE_LIBSQLITE3\nheader\n#else // USE_LIBSQLITE3\n#endif\n",
            "sqlite3ext.h": b"#ifndef USE_LIBSQLITE3\nextension\n#else // USE_LIBSQLITE3\n#endif\n",
            "sqlite3.go": b"#cgo CFLAGS: -DSQLITE_THREADSAFE=1\n",
            "sqlite3_other.go": b"#cgo CFLAGS: -I.\n",
        }
        pins = []
        for name, data in sources.items():
            (self.module / name).write_bytes(data)
            pins.append(hashlib.sha256(data).hexdigest() + "  " + name)
        (self.root / "sqlite-source.sha256").write_text("\n".join(pins))
        self.llvm = self.root / "llvm"
        self.llvm.mkdir()
        for name in ("clang", "llvm-ar", "ld.lld", "llvm-profdata", "go"):
            path = self.llvm / name
            path.write_bytes(b"fake Linux executable: " + name.encode())
            path.chmod(0o755)
        (self.root / "profiles").mkdir()
        shutil.copyfile(ROOT / "profiles/go.pprof", self.root / "profiles/go.pprof")
        self.commands = []
        self.cpu_status = 0
        self.tests_fail = False
        self.elf_extra = ""
        self.symbols_extra = ""
        self.missing_archive = False
        self.wrong_extraction = False
        self.invalid_bitcode = False
        self.compile_warning = ""
        self.profile_change_on_link = False
        self.go_profile_change_at = None
        self.pprof_status = 0
        self.real_pprof = False
        (self.root / "app").mkdir()
        (self.root / "app/cfeed.c").write_text("int cfeed_fixture;\n")
        patches = [patch.object(self.b, "ROOT", self.root),
                   patch.object(self.b.platform, "system", return_value="Linux"),
                   patch.object(self.b.platform, "machine", return_value="x86_64"),
                   patch.object(self.b.shutil, "which", return_value="/usr/bin/binutils"),
                   patch.dict(os.environ, {"PATH": os.environ["PATH"], "LLVM_BIN": str(self.llvm),
                                           "GO": str(self.llvm / "go")}, clear=True),
                   patch.object(self.b.subprocess, "run", side_effect=self.external)]
        for item in patches:
            item.start()
            self.addCleanup(item.stop)

    def external(self, command, **kwargs):
        env = dict(kwargs["env"])
        self.commands.append((command, env))
        tool = Path(command[0]).name
        stdout, code = "", 0
        if command[1:] == ["--version"]:
            stdout = "LLVM version 23.1.3\n"
        elif tool == "go" and command[1:] == ["version"]:
            stdout = "go version go1.27.2 linux/amd64\n"
        elif tool == "clang" and "-dM" in command:
            stdout = "".join("#define " + name + " " + value + "\n" for name, value in
                             (("__clang_major__", "23"), ("__clang_minor__", "1"),
                              ("__clang_patchlevel__", "3"), ("__linux__", "1"), ("__x86_64__", "1")))
        elif tool == "go" and command[1:3] == ["tool", "pprof"]:
            if self.real_pprof:
                kwargs["env"] = dict(LOCAL_ENV, **kwargs["env"])
                kwargs["env"].update(GOPROXY="off", GOSUMDB="off")
                return REAL_SUBPROCESS_RUN([LOCAL_GO, *command[1:]], **kwargs)
            code = self.pprof_status
            stdout = "invalid CPU profile" if code else "Type: cpu\n"
        elif tool == "go" and command[1:3] == ["mod", "download"]:
            pass
        elif tool == "go" and command[1:3] == ["mod", "verify"]:
            pass
        elif tool == "go" and command[1] == "list":
            stdout = json.dumps({"Version": "v1.14.52", "Dir": str(self.module)})
        elif tool == "go" and command[1] == "build":
            output = Path(command[command.index("-o") + 1])
            output.write_bytes(b"built server or probe")
            if output.name == "server.tmp":
                archive = self.root / "build/libsqlite3.a"
                thin = "-flto=thin" in env["CGO_CFLAGS"]
                (self.root / "build/server.link.map").write_text(
                    "lto.tmp:(.text.sqlite3_step)" if thin else
                    "unrelated archive" if self.missing_archive else str(archive) + "(sqlite3.o)")
                if thin:
                    selected = str(archive) + "(sqlite3.o)"
                    if self.missing_archive:
                        selected = "/system/libsqlite3.a(sqlite3.o)"
                    if self.wrong_extraction:
                        selected += ".wrong"
                    (self.root / "build/server.why-extract.tsv").write_text(
                        "reference\textracted\tsymbol\n/tmp/go-link/000001.o\t" + selected + "\tsqlite3_step\n")
                if self.go_profile_change_at == "link":
                    self.go_profile.write_bytes(b"changed at link")
                if self.profile_change_on_link:
                    (self.root / "profiles/sqlite.profraw").write_bytes(b"changed during link")
        elif tool == "v3probe":
            code = self.cpu_status
        elif tool == "clang" and "-c" in command:
            thin = "-flto=thin" in command
            Path(command[command.index("-o") + 1]).write_bytes(
                b"BC\xc0\xdecompiled SQLite" if thin and not self.invalid_bitcode else b"compiled SQLite")
            stdout = self.compile_warning
            code = 1 if self.compile_warning else 0
        elif tool == "clang" and "-emit-llvm" in command:
            self.assertIn("-x", command)
            self.assertIn("ir", command)
            self.assertEqual(command[-2:], ["-o", "/dev/null"])
        elif tool == "llvm-ar" and command[1] == "rcs":
            Path(command[2]).write_bytes(b"archive of SQLite")
        elif tool == "llvm-ar" and command[1] == "t":
            stdout = "sqlite3.o\n"
        elif tool == "llvm-profdata" and command[1] == "merge":
            Path(command[command.index("-o") + 1]).write_bytes(b"merged fixture profile")
        elif tool == "llvm-profdata" and command[1] == "show":
            source = "/training/sqlite3.c" if command[-1].endswith(".profraw") else str(self.root / "build/sqlite3.c")
            stdout = ("Instrumentation level: IR\nMaximum function count: 42\nsqlite3_open_v2\nsqlite3_step\n"
                      + f"  {source};sqlite3Static:\n    Hash: 0x1234\n    Counters: 2\n    Block counts: [42, 0]\n")
        elif tool == "readelf":
            stdout = "Advanced Micro Devices X86-64\n" + self.elf_extra
        elif tool == "nm":
            stdout = "000000 T sqlite3_libversion\n000001 T sqlite3_threadsafe\n" + self.symbols_extra
        elif tool == "go" and command[1:3] == ["version", "-m"]:
            pass
        elif tool == "go" and command[1] == "test":
            if self.go_profile_change_at == "test":
                self.go_profile.write_bytes(b"changed at test")
            code = 1 if self.tests_fail else 0
        else:
            raise AssertionError("unexpected external command: " + repr(command))
        return subprocess.CompletedProcess(command, code, stdout=stdout)

    def test_non_pgo_non_lto_build_without_profile_tools_and_cached_sqlite(self):
        (self.llvm / "llvm-profdata").unlink()
        self.b.main(["--no-pgo", "--no-thinlto", "--no-go-pgo"])
        self.assertEqual((self.root / "bin/server").read_bytes(), b"built server or probe")
        builds = [(cmd, env) for cmd, env in self.commands if cmd[1:2] == ["build"]]
        for command, env in builds:
            self.assertIn("-pgo=off", command)
            self.assertIn("-p=1", command)
            self.assertNotIn("-a", command)
            self.assertNotIn("-fprofile", env["CGO_CFLAGS"])
            self.assertNotIn("-flto", env["CGO_CFLAGS"])
            self.assertNotIn("thinlto", env["CGO_LDFLAGS"])
            self.assertNotIn("sqlite_llvm_pgo", str(command))
        self.assertEqual(builds[-1][1]["GOAMD64"], "v3")
        self.commands.clear()
        self.b.main(["--no-pgo", "--no-thinlto", "--no-go-pgo"])
        self.assertFalse(any(Path(cmd[0]).name == "clang" and "-c" in cmd for cmd, _ in self.commands))

    def test_unsupported_cpu_disables_only_sqlite_pgo_and_keeps_default_go_profile(self):
        self.cpu_status = 1
        self.b.main(["--no-pgo", "--no-thinlto"])
        commands = [env for cmd, env in self.commands if cmd[1:2] == ["build"]]
        self.assertEqual(commands[-1]["GOAMD64"], "v1")
        self.assertIn("-march=x86-64 ", commands[-1]["CGO_CFLAGS"])
        receipt = json.loads((self.root / "build/build-receipt.json").read_text())
        self.assertEqual(receipt["go_pgo"]["path"], str((self.root / "profiles/go.pprof").resolve()))
        self.assertIn("-pgo=" + receipt["go_pgo"]["path"], receipt["go_options"])

    def test_go_test_failure_never_publishes_server(self):
        self.tests_fail = True
        with self.assertRaisesRegex(ValueError, "command failed"):
            self.b.main(["--no-pgo", "--test"])
        self.assertFalse((self.root / "bin/server").exists())
        self.assertFalse((self.root / "bin/server.tmp").exists())


    def prepare_profile(self):
        directory = self.root / "profiles"
        directory.mkdir(exist_ok=True)
        raw, _, _, manifest = Contracts.fixture(self, directory)
        manifest["sqlite_sources"] = dict(line.split()[::-1] for line in
                                           (self.root / "sqlite-source.sha256").read_text().splitlines())
        manifest["sqlite_flags"] = ["--no-default-config", "-O2", "-fPIC", "-pthread", "-fno-common",
                                    "-Werror", "-march=x86-64-v3", "-DSQLITE_THREADSAFE=1"]
        (directory / "manifest.json").write_text(json.dumps(manifest))

    def test_explicit_pgo_instruments_neither_cgo_nor_go(self):
        self.prepare_profile()
        self.b.main(["--pgo", "--no-thinlto", "--no-go-pgo", "--test"])
        compilation = [cmd for cmd, _ in self.commands if Path(cmd[0]).name == "clang" and "-c" in cmd]
        self.assertEqual(len(compilation), 1)
        self.assertTrue(any(flag.startswith("-fprofile-use=") for flag in compilation[0]))
        self.assertIn("-Werror=profile-instr-out-of-date", compilation[0])
        self.assertIn("-Werror=profile-instr-unprofiled", compilation[0])
        self.assertFalse(any(flag.startswith("-flto") for flag in compilation[0]))
        for command, env in self.commands:
            if Path(command[0]).name == "go" and command[1] in ("build", "test"):
                self.assertIn("-pgo=off", command)
                self.assertNotIn("-fprofile", env["CGO_CFLAGS"])
                self.assertNotIn("-fprofile", env["CGO_LDFLAGS"])
                self.assertNotIn("sqlite_llvm_pgo", str(command))
        self.assertTrue((self.root / "bin/server").is_file())

    def test_default_and_pgo_only_unsupported_cpu_fail_with_no_pgo_guidance(self):
        self.prepare_profile()
        self.cpu_status = 1
        for options in ([], ["--no-thinlto"]):
            with self.subTest(options=options):
                self.commands.clear()
                with self.assertRaisesRegex(ValueError, "x86-64-v3.*--no-pgo"):
                    self.b.main(options)
                self.assertFalse((self.root / "bin/server").exists())
                self.assertFalse((self.root / "build/build-receipt.json").exists())
                self.assertFalse(any(Path(cmd[0]).name == "clang" and "-c" in cmd
                                     for cmd, _ in self.commands))

    def test_probe_server_and_tests_explicitly_link_non_pie_in_all_modes(self):
        self.prepare_profile()
        for options in (["--test"], ["--baseline", "--test"],
                        ["--no-pgo", "--test"], ["--no-thinlto", "--test"],
                        ["--no-pgo", "--no-thinlto", "--test"],
                        ["--baseline", "--no-thinlto", "--test"]):
            with self.subTest(options=options):
                self.commands.clear()
                self.b.main(options)
                links = [(cmd, env) for cmd, env in self.commands
                         if Path(cmd[0]).name == "go" and cmd[1] in ("build", "test")]
                self.assertEqual(len(links), 2 if "--baseline" in options else 3)
                for command, env in links:
                    go_ldflags = shlex.split(next(arg[len("-ldflags="):]
                                                 for arg in command if arg.startswith("-ldflags=")))
                    self.assertIn("-linkmode=external", go_ldflags)
                    external_flags = shlex.split(go_ldflags[go_ldflags.index("-extldflags") + 1])
                    # Go's optional compiler-support probe must not decide whether
                    # non-PIC Go objects reach a default-PIE external linker.
                    self.assertIn("-no-pie", external_flags)
                    # Cgo performs intermediate links without Go's -extldflags.
                    self.assertIn("-no-pie", shlex.split(env["CGO_LDFLAGS"]))
                    self.assertNotIn("-no-pie", shlex.split(env["CGO_CFLAGS"]))
                    self.assertNotIn("-pie", external_flags)
                receipt = json.loads((self.root / "build/sqlite-build.json").read_text())
                self.assertIn("-no-pie", receipt["identity"]["link_flags"])
                compilation = [cmd for cmd, _ in self.commands
                               if Path(cmd[0]).name == "clang" and "-c" in cmd]
                for command in compilation:
                    self.assertNotIn("-no-pie", command)

    def test_thinlto_applies_to_sqlite_cgo_and_lld_with_one_backend_job(self):
        self.prepare_profile()
        manifest = (self.root / "profiles/manifest.json").read_bytes()
        self.b.main(["--pgo", "--thinlto", "--test"])
        compilation = [cmd for cmd, _ in self.commands if Path(cmd[0]).name == "clang" and "-c" in cmd]
        self.assertEqual(len(compilation), 1)
        for flag in ("-O2", "-flto=thin", "-DSQLITE_THREADSAFE=1",
                     "-Werror=profile-instr-unprofiled", "-Werror=profile-instr-out-of-date",
                     "-Werror=backend-plugin", "-pgo-warn-missing-function"):
            self.assertIn(flag, compilation[0])
        self.assertTrue(any(flag.startswith("-fprofile-use=") for flag in compilation[0]))
        for command, env in self.commands:
            if Path(command[0]).name == "go" and command[1] in ("build", "test"):
                self.assertIn("-pgo=off" if command[1] == "build" and
                              command[command.index("-o") + 1].endswith("/v3probe") else
                              "-pgo=" + str((self.root / "profiles/go.pprof").resolve()), command)
                self.assertIn("-flto=thin", env["CGO_CFLAGS"])
                self.assertNotIn("-fprofile", env["CGO_CFLAGS"])
                for flag in ("-O2", "-flto=thin", "-Wl,--thinlto-jobs=1", "-Wl,--lto-O2",
                             "--ld-path=" + str((self.llvm / "ld.lld").resolve())):
                    self.assertIn(flag, env["CGO_LDFLAGS"])
                    self.assertIn(flag, " ".join(command))
                if "server.tmp" in " ".join(command):
                    self.assertIn("--why-extract=", " ".join(command))
        self.assertFalse(any(Path(cmd[0]).name == "readelf" and cmd[-1].endswith("sqlite3.o")
                             for cmd, _ in self.commands))
        self.assertTrue(any("-emit-llvm" in cmd for cmd, _ in self.commands))
        self.assertTrue(any(Path(cmd[0]).name == "readelf" and "-SW" in cmd
                            and cmd[-1].endswith("server.tmp") for cmd, _ in self.commands))
        self.assertEqual((self.root / "profiles/manifest.json").read_bytes(), manifest)
        receipt = json.loads((self.root / "build/build-receipt.json").read_text())
        self.assertEqual(receipt["mode"], "pgo-thinlto")
        self.assertTrue(receipt["experimental_compile_use_change"])
        self.assertEqual(receipt["thinlto_backend_jobs"], 1)
        self.assertEqual(receipt["source_sha256"]["app/cfeed.c"],
                         hashlib.sha256(b"int cfeed_fixture;\n").hexdigest())
        self.assertEqual(receipt["binary_sha256"], self.b.sha(self.root / "bin/server"))

    def test_cache_and_receipts_distinguish_control_thinlto_and_jobs(self):
        self.prepare_profile()
        receipts = []
        builds = []
        for options in (["--pgo", "--no-thinlto"], [], [], ["--pgo", "--no-thinlto"]):
            self.commands.clear()
            self.b.main(options)
            receipts.append(json.loads((self.root / "build/sqlite-build.json").read_text()))
            builds.append(json.loads((self.root / "build/build-receipt.json").read_text()))
            compiled = any(Path(cmd[0]).name == "clang" and "-c" in cmd for cmd, _ in self.commands)
            self.assertEqual(compiled, len(receipts) != 3)
        self.assertNotEqual(receipts[0]["key"], receipts[1]["key"])
        self.assertEqual(receipts[1]["key"], receipts[2]["key"])
        self.assertEqual(receipts[0]["key"], receipts[3]["key"])
        self.assertIn("-flto=thin", receipts[1]["identity"]["flags"])
        self.assertIn("-Wl,--thinlto-jobs=1", receipts[1]["identity"]["link_flags"])
        self.assertNotIn("-flto=thin", receipts[0]["identity"]["flags"])
        self.assertEqual(builds[0]["source_sha256"], builds[1]["source_sha256"])
        self.assertEqual(builds[0]["training_contract"], builds[1]["training_contract"])
        self.assertEqual(builds[0]["raw_profile_sha256"], builds[1]["raw_profile_sha256"])
        self.assertEqual(builds[0]["profile_manifest_sha256"], builds[1]["profile_manifest_sha256"])
        final = json.loads((self.root / "build/build-receipt.json").read_text())
        self.assertEqual(final["mode"], "pgo")
        self.assertFalse(final["experimental_compile_use_change"])

    def test_four_modes_have_distinct_cache_keys_and_accurate_receipts(self):
        self.prepare_profile()
        manifest = (self.root / "profiles/manifest.json").read_bytes()
        keys = set()
        cgo_ids = set()
        for options, pgo, thinlto, mode in (
                ([], True, True, "pgo-thinlto"),
                (["--no-pgo"], False, True, "thinlto"),
                (["--no-pgo", "--no-thinlto"], False, False, "non-pgo"),
                (["--no-thinlto"], True, False, "pgo")):
            with self.subTest(mode=mode):
                self.commands.clear()
                self.b.main(options + ["--test"])
                receipt = json.loads((self.root / "build/build-receipt.json").read_text())
                identity = receipt["sqlite_build"]["identity"]
                keys.add(receipt["sqlite_build"]["key"])
                cgo_ids.add(next(flag for flag in receipt["cgo_cflags"].split()
                                 if flag.startswith("-DGO_SQLITE_BUILD_ID=")))
                self.assertEqual(receipt["mode"], mode)
                self.assertEqual(receipt["thinlto_backend_jobs"], 1 if thinlto else None)
                self.assertEqual(receipt["experimental_compile_use_change"], thinlto)
                self.assertEqual(receipt["archive_proof"],
                                 "server.why-extract.tsv" if thinlto else "server.link.map")
                self.assertEqual("-flto=thin" in identity["flags"], thinlto)
                self.assertEqual("-Wl,--thinlto-jobs=1" in identity["link_flags"], thinlto)
                self.assertEqual(identity["profdata_sha256"] is not None, pgo)
                self.assertEqual("PROFDATA" in receipt["tools"], pgo)
                for field in ("training_contract", "raw_profile_sha256", "profile_manifest_sha256"):
                    self.assertEqual(receipt[field] is not None, pgo)
                compilations = [cmd for cmd, _ in self.commands
                                if Path(cmd[0]).name == "clang" and "-c" in cmd]
                self.assertEqual(len(compilations), 1)
                self.assertEqual(any(flag.startswith("-fprofile-use=") for flag in compilations[0]), pgo)
                for command, env in self.commands:
                    if Path(command[0]).name == "go" and command[1] in ("build", "test"):
                        self.assertIn("-pgo=off" if command[1] == "build" and
                                      command[command.index("-o") + 1].endswith("/v3probe") else
                                      "-pgo=" + str((self.root / "profiles/go.pprof").resolve()), command)
                        self.assertNotIn("sqlite_llvm_pgo", str(command))
                        self.assertNotIn("-fprofile", env["CGO_CFLAGS"])
                        self.assertNotIn("-fprofile", env["CGO_LDFLAGS"])
                        self.assertEqual("-flto=thin" in env["CGO_CFLAGS"], thinlto)
                        self.assertEqual("-flto=thin" in env["CGO_LDFLAGS"], thinlto)
                self.commands.clear()
                self.b.main(options)
                self.assertFalse(any(Path(cmd[0]).name == "clang" and "-c" in cmd
                                     for cmd, _ in self.commands))
                self.assertEqual((self.root / "profiles/manifest.json").read_bytes(), manifest)
        self.assertEqual(len(keys), 4)
        self.assertEqual(len(cgo_ids), 4)

    def test_non_pgo_cpu_matrix_needs_no_profile_or_profdata(self):
        shutil.rmtree(self.root / "profiles")
        (self.llvm / "llvm-profdata").unlink()
        for cpu_status in (0, 1):
            for thinlto in (False, True):
                with self.subTest(cpu_status=cpu_status, thinlto=thinlto):
                    self.cpu_status = cpu_status
                    self.commands.clear()
                    self.b.main(["--no-pgo", "--no-go-pgo", "--thinlto" if thinlto else "--no-thinlto"])
                    receipt = json.loads((self.root / "build/build-receipt.json").read_text())
                    final = [env for cmd, env in self.commands if cmd[1:2] == ["build"]][-1]
                    self.assertEqual(final["GOAMD64"], "v3" if cpu_status == 0 else "v1")
                    self.assertEqual(receipt["mode"], "thinlto" if thinlto else "non-pgo")
                    self.assertFalse(any(Path(cmd[0]).name == "llvm-profdata" for cmd, _ in self.commands))

    def test_baseline_skips_cpu_probe_and_profile_tools(self):
        shutil.rmtree(self.root / "profiles")
        (self.llvm / "llvm-profdata").unlink()
        for options, thinlto in ((["--baseline"], True), (["--baseline", "--no-thinlto"], False)):
            with self.subTest(options=options):
                self.commands.clear()
                self.b.main(options)
                self.assertFalse(any(Path(cmd[0]).name == "v3probe" for cmd, _ in self.commands))
                receipt = json.loads((self.root / "build/build-receipt.json").read_text())
                self.assertIsNone(receipt["training_contract"])
                self.assertIsNone(receipt["go_pgo"])
                self.assertIn("-pgo=off", receipt["go_options"])
                self.assertFalse(any(cmd[1:3] == ["tool", "pprof"] for cmd, _ in self.commands))
                self.assertIn("-march=x86-64 ", receipt["cgo_cflags"])
                self.assertEqual("-flto=thin" in receipt["cgo_cflags"], thinlto)

    def test_default_and_pgo_only_require_valid_profile_even_with_cached_archive(self):
        self.prepare_profile()
        raw = self.root / "profiles/sqlite.profraw"
        manifest = self.root / "profiles/manifest.json"
        raw_bytes, manifest_bytes = raw.read_bytes(), manifest.read_bytes()
        for options in ([], ["--no-thinlto"]):
            for failure in ("missing_manifest", "missing_raw", "corrupt_raw"):
                with self.subTest(options=options, failure=failure):
                    raw.write_bytes(raw_bytes)
                    manifest.write_bytes(manifest_bytes)
                    self.b.main(options)
                    if failure == "missing_manifest":
                        manifest.unlink()
                    elif failure == "missing_raw":
                        raw.unlink()
                    else:
                        raw.write_bytes(b"corrupt")
                    self.commands.clear()
                    with self.assertRaisesRegex(ValueError, "manifest|raw"):
                        self.b.main(options)
                    self.assertFalse((self.root / "bin/server").exists())
                    self.assertFalse((self.root / "build/build-receipt.json").exists())
                    self.assertFalse(any(Path(cmd[0]).name == "clang" and "-c" in cmd
                                         for cmd, _ in self.commands))

    def test_backend_jobs_change_invalidates_sqlite_cache(self):
        self.prepare_profile()
        # Simulate a cache left by a different experimental jobs setting.
        # Production exposes no jobs override and always uses one backend job.
        with patch.object(self.b, "THIN_LINK", ["-flto=thin", "-Wl,--thinlto-jobs=2", "-Wl,--lto-O2"]):
            self.b.main(["--pgo", "--thinlto"])
        previous = json.loads((self.root / "build/sqlite-build.json").read_text())
        self.commands.clear()
        self.b.main(["--pgo", "--thinlto"])
        current = json.loads((self.root / "build/sqlite-build.json").read_text())
        self.assertNotEqual(previous["key"], current["key"])
        self.assertIn("-Wl,--thinlto-jobs=1", current["identity"]["link_flags"])
        self.assertTrue(any(Path(cmd[0]).name == "clang" and "-c" in cmd for cmd, _ in self.commands))

    def test_thinlto_profile_strictness_and_no_fallback(self):
        self.prepare_profile()
        manifest_path = self.root / "profiles/manifest.json"
        original = manifest_path.read_text()
        for mutation, message in (("missing", "manifest"), ("lto_flags", "sqlite_flags"),
                                  ("raw", "raw"), ("stale_warning", "profile-instr-out-of-date"),
                                  ("missing_warning", "profile-instr-unprofiled"),
                                  ("backend_warning", "backend-plugin"),
                                  ("link_mutation", "raw")):
            with self.subTest(mutation=mutation):
                manifest_path.write_text(original)
                raw = self.root / "profiles/sqlite.profraw"
                raw.write_bytes(b"fixture; LLVM itself validates instrumentation data on Linux")
                self.compile_warning = ""
                self.profile_change_on_link = False
                if mutation == "missing":
                    manifest_path.unlink()
                elif mutation == "lto_flags":
                    manifest = json.loads(original)
                    manifest["sqlite_flags"].append("-flto=thin")
                    manifest_path.write_text(json.dumps(manifest))
                elif mutation == "raw":
                    raw.write_bytes(b"corrupt")
                elif mutation.endswith("_warning"):
                    (self.root / "build/sqlite-build.json").unlink(missing_ok=True)
                    self.compile_warning = "error: incompatible profile [-Werror=" + message + "]"
                else:
                    self.profile_change_on_link = True
                self.commands.clear()
                with self.assertRaisesRegex(ValueError, message):
                    self.b.main([])
                self.assertFalse((self.root / "bin/server").exists())
                self.assertFalse((self.root / "build/build-receipt.json").exists())
                self.assertFalse(any(Path(cmd[0]).name == "clang" and "-c" in cmd
                                     and "-flto=thin" not in cmd for cmd, _ in self.commands))

    def test_thinlto_rejects_non_bitcode_and_wrong_archive_extraction(self):
        self.prepare_profile()
        for options in ([], ["--no-pgo"]):
            for attribute, message in (("invalid_bitcode", "bitcode"),
                                       ("missing_archive", "archive missing"),
                                       ("wrong_extraction", "archive missing")):
                with self.subTest(options=options, attribute=attribute):
                    self.invalid_bitcode = self.missing_archive = self.wrong_extraction = False
                    setattr(self, attribute, True)
                    (self.root / "build/sqlite-build.json").unlink(missing_ok=True)
                    with self.assertRaisesRegex(ValueError, message):
                        self.b.main(options)
                    self.assertFalse((self.root / "bin/server").exists())

    def test_thinlto_keeps_final_elf_sqlite_and_runtime_checks(self):
        self.prepare_profile()
        for elf, symbols, message in (
                ("NEEDED Shared library: [libsqlite3.so.0]", "", "dynamic SQLite"),
                ("__llvm_prf_cnts", "", "training counters"),
                ("", "000002 T __llvm_profile_write_file\n", "training runtime"),
                ("", "000002 T __gcov_init\n", "training runtime")):
            with self.subTest(message=message, symbols=symbols):
                self.elf_extra, self.symbols_extra = elf, symbols
                with self.assertRaisesRegex(ValueError, message):
                    self.b.main(["--pgo", "--thinlto"])
                self.assertFalse((self.root / "bin/server").exists())


    def test_dynamic_sqlite_training_runtime_and_unlinked_archive_fail(self):
        self.prepare_profile()
        cases = [("elf_extra", "NEEDED Shared library: [libsqlite3.so.0]", "dynamic SQLite"),
                 ("symbols_extra", "000002 T __llvm_profile_write_file\n", "training runtime"),
                 ("symbols_extra", "000002 T goal_llvm_profile_export\n", "training runtime"),
                 ("missing_archive", True, "archive missing")]
        for options in ([], ["--no-pgo"], ["--no-thinlto"], ["--no-pgo", "--no-thinlto"]):
            for name, value, message in cases:
                with self.subTest(options=options, name=name, value=value):
                    self.elf_extra, self.symbols_extra, self.missing_archive = "", "", False
                    setattr(self, name, value)
                    with self.assertRaisesRegex(ValueError, message):
                        self.b.main(options)
                    self.assertFalse((self.root / "bin/server").exists())
                    self.assertFalse((self.root / "bin/server.tmp").exists())


    def test_default_go_profile_reaches_app_tests_and_receipt_but_not_probe(self):
        self.prepare_profile()
        self.b.main(["--test"])
        path = str((self.root / "profiles/go.pprof").resolve())
        go_commands = [cmd for cmd, _ in self.commands
                       if Path(cmd[0]).name == "go" and cmd[1] in ("build", "test")]
        self.assertEqual(len(go_commands), 3)
        self.assertIn("-pgo=off", go_commands[0])
        for command in go_commands[1:]:
            self.assertIn("-pgo=" + path, command)
        receipt = json.loads((self.root / "build/build-receipt.json").read_text())
        self.assertEqual(receipt["go_pgo"], {"path": path, "sha256":
                         "cffbedaaa8f7208e6cce79844a858e8ae2f08a3a14079cdc345f0968e63eb6ff"})
        self.assertEqual(receipt["training_contract"]["go"]["pgo"], "off")
        self.assertTrue((self.root / "bin/server").is_file())

    def test_default_go_profile_integrity_failures_never_fall_back_even_with_cache(self):
        profile = self.root / "profiles/go.pprof"
        original = profile.read_bytes()
        for state in ("missing", "empty", "mismatch", "directory", "symlink"):
            with self.subTest(state=state):
                profile.write_bytes(original)
                self.b.main(["--no-pgo"])
                if state == "missing":
                    profile.unlink()
                elif state == "empty":
                    profile.write_bytes(b"")
                elif state == "mismatch":
                    profile.write_bytes(b"corrupt bundled profile")
                elif state == "directory":
                    profile.unlink()
                    profile.mkdir()
                elif state == "symlink":
                    profile.unlink()
                    profile.symlink_to(ROOT / "profiles/go.pprof")
                self.commands.clear()
                with self.assertRaisesRegex(ValueError, "Go PGO profile"):
                    self.b.main(["--no-pgo"])
                self.assertEqual(self.commands, [])
                self.assertFalse((self.root / "bin/server").exists())
                self.assertFalse((self.root / "build/build-receipt.json").exists())
                if profile.is_dir():
                    profile.rmdir()
                else:
                    profile.unlink(missing_ok=True)

    def test_default_go_profile_changed_during_link_or_tests_cannot_publish(self):
        self.go_profile = self.root / "profiles/go.pprof"
        original = self.go_profile.read_bytes()
        for stage in ("link", "test"):
            with self.subTest(stage=stage):
                self.go_profile.write_bytes(original)
                self.go_profile_change_at = stage
                with self.assertRaisesRegex(ValueError, "Go PGO profile.*mismatch"):
                    self.b.main(["--no-pgo", "--test"])
                for name in ("bin/server", "bin/server.tmp", "build/build-receipt.json"):
                    self.assertFalse((self.root / name).exists(), name)

    def go_profile_options(self, fixture="positive"):
        self.go_profile = self.root / "current100k cpu.pprof"
        self.go_profile.write_bytes((FIXTURES / (fixture + ".pprof")).read_bytes())
        return ["--go-pgo", str(self.go_profile), "--go-pgo-sha256",
                hashlib.sha256(self.go_profile.read_bytes()).hexdigest()]


    def test_go_pgo_reaches_app_and_tests_but_not_cpu_probe_in_matched_pair(self):
        self.prepare_profile()
        profile_options = self.go_profile_options()
        for options in (["--pgo", "--thinlto", "--no-go-pgo", "--test"],
                        ["--pgo", "--thinlto", "--test", *profile_options]):
            with self.subTest(options=options):
                self.commands.clear()
                self.b.main(options)
                enabled = "--go-pgo" in options
                expected = "-pgo=" + str(self.go_profile.resolve()) if enabled else "-pgo=off"
                go_commands = [cmd for cmd, _ in self.commands
                               if Path(cmd[0]).name == "go" and cmd[1] in ("build", "test")]
                self.assertEqual(len(go_commands), 3)
                self.assertIn("-pgo=off", go_commands[0])
                for cmd in go_commands[1:]:
                    self.assertIn(expected, cmd)
                    self.assertEqual(len([arg for arg in cmd if arg.startswith("-pgo=")]), 1)
                pprof = [cmd for cmd, _ in self.commands if cmd[1:3] == ["tool", "pprof"]]
                self.assertEqual(len(pprof), int(enabled))
                if enabled:
                    self.assertIn("-sample_index=cpu", pprof[0])
                    self.assertIn("-symbolize=none", pprof[0])
                    self.assertEqual(pprof[0][-1], str(self.go_profile.resolve()))
                receipt = json.loads((self.root / "build/build-receipt.json").read_text())
                self.assertEqual(receipt["go_pgo"], {
                    "path": str(self.go_profile.resolve()), "sha256": profile_options[-1]
                } if enabled else None)
                self.assertIn(expected, receipt["go_options"])
                self.assertEqual(receipt["training_contract"]["go"]["pgo"], "off")
                self.assertEqual(receipt["thinlto_backend_jobs"], 1)
                self.assertIn("-no-pie", receipt["cgo_ldflags"])
                if not enabled:
                    control = receipt
                else:
                    self.assertEqual(
                        [arg for arg in control["go_options"] if not arg.startswith("-pgo=")],
                        [arg for arg in receipt["go_options"] if not arg.startswith("-pgo=")])
                    for key in ("source_sha256", "sqlite_sources", "training_contract",
                                "profile_manifest_sha256", "raw_profile_sha256", "sqlite_build",
                                "cgo_cflags", "cgo_ldflags"):
                        self.assertEqual(control[key], receipt[key], key)


    def test_custom_go_pgo_preserves_all_sqlite_lto_modes(self):
        self.prepare_profile()
        profile = self.go_profile_options()
        for options, mode in (([], "pgo-thinlto"), (["--no-thinlto"], "pgo"),
                              (["--no-pgo"], "thinlto"),
                              (["--no-pgo", "--no-thinlto"], "non-pgo")):
            with self.subTest(options=options):
                self.commands.clear()
                self.b.main([*options, *profile, "--test"])
                receipt = json.loads((self.root / "build/build-receipt.json").read_text())
                self.assertEqual(receipt["mode"], mode)
                self.assertEqual(receipt["go_pgo"]["sha256"], profile[-1])
                for command, _ in self.commands:
                    if Path(command[0]).name == "go" and command[1] in ("build", "test"):
                        expected = "-pgo=" + str(self.go_profile.resolve())
                        # The probe output, unlike the app output, lives in v3probe.
                        if command[1] == "build" and command[command.index("-o") + 1].endswith("/v3probe"):
                            expected = "-pgo=off"
                        self.assertIn(expected, command)

    def test_go_pgo_relative_path_is_absolute_for_app_build(self):
        options = self.go_profile_options()
        options[1] = self.go_profile.name
        previous = Path.cwd()
        try:
            os.chdir(self.root)
            self.b.main(["--no-pgo", "--no-thinlto", *options])
        finally:
            os.chdir(previous)
        self.assertTrue(any("-pgo=" + str(self.go_profile.resolve()) in cmd for cmd, _ in self.commands))


    def test_bad_go_profile_fails_before_building_and_removes_old_success(self):
        for state in ("missing", "empty", "mismatch", "directory", "symlink"):
            with self.subTest(state=state):
                options = self.go_profile_options()
                if state == "missing":
                    self.go_profile.unlink()
                elif state == "empty":
                    self.go_profile.write_bytes(b"")
                elif state == "mismatch":
                    options[-1] = "0" * 64
                elif state == "directory":
                    self.go_profile.unlink()
                    self.go_profile.mkdir()
                elif state == "symlink":
                    self.go_profile.unlink()
                    self.go_profile.symlink_to(self.module / "sqlite3.go")
                binary = self.root / "bin/server"
                binary.parent.mkdir(exist_ok=True)
                binary.write_bytes(b"stale server")
                (self.root / "build").mkdir(exist_ok=True)
                (self.root / "build/build-receipt.json").write_text('{"old": true}')
                self.commands.clear()
                with self.assertRaisesRegex(ValueError, "Go PGO profile"):
                    self.b.main(["--no-pgo", "--no-thinlto", *options])
                self.assertFalse(binary.exists())
                self.assertFalse((self.root / "build/build-receipt.json").exists())
                self.assertEqual(self.commands, [])
                if self.go_profile.is_dir():
                    self.go_profile.rmdir()
                else:
                    self.go_profile.unlink(missing_ok=True)


    def test_pprof_rejection_prevents_native_build_and_publication(self):
        options = self.go_profile_options()
        self.pprof_status = 1
        with self.assertRaisesRegex(ValueError, "invalid CPU profile"):
            self.b.main(["--no-pgo", "--no-thinlto", *options])
        self.assertFalse(any(cmd[1:2] == ["build"] for cmd, _ in self.commands))
        self.assertFalse((self.root / "bin/server").exists())
        self.assertFalse((self.root / "build/build-receipt.json").exists())


    @unittest.skipUnless(LOCAL_GO, "real pprof checks require an installed Go toolchain")
    def test_real_zero_weight_and_wrong_unit_profiles_fail_before_building(self):
        self.real_pprof = True
        for fixture, message in (("zero", "positive CPU weight"),
                                 ("wrong-unit", "cpu/nanoseconds"),
                                 ("count-first-zero", "positive compiler-selected weight"),
                                 ("cpu-first-zero", "positive CPU weight"),
                                 ("count-first-zero-cpu", "positive CPU weight")):
            with self.subTest(fixture=fixture):
                options = self.go_profile_options(fixture)
                # All are well-formed profiles accepted by Go's real pprof.
                parsed = REAL_SUBPROCESS_RUN(
                    [LOCAL_GO, "tool", "pprof", "-top", "-nodecount=1",
                     "-sample_index=cpu", "-symbolize=none", str(self.go_profile)],
                    capture_output=True, text=True,
                    env=dict(LOCAL_ENV, GOTOOLCHAIN="local", GOPROXY="off", GOSUMDB="off",
                             GOENV="off", GOWORK="off", GOTELEMETRY="off"))
                self.assertEqual(parsed.returncode, 0, parsed.stdout + parsed.stderr)
                binary = self.root / "bin/server"
                binary.parent.mkdir(exist_ok=True)
                binary.write_bytes(b"stale server")
                (self.root / "build").mkdir(exist_ok=True)
                (self.root / "build/build-receipt.json").write_text('{"old": true}')
                self.commands.clear()
                with self.assertRaisesRegex(ValueError, message):
                    self.b.main(["--no-pgo", "--no-thinlto", *options])
                self.assertFalse(any(cmd[1:2] in (["build"], ["mod"])
                                     for cmd, _ in self.commands))
                for name in ("bin/server", "bin/server.tmp", "build/build-receipt.json"):
                    self.assertFalse((self.root / name).exists(), name)


    @unittest.skipUnless(LOCAL_GO, "real pprof checks require an installed Go toolchain")
    def test_real_positive_cpu_profile_reaches_app_and_tests(self):
        self.real_pprof = True
        for fixture in ("positive", "count-first-positive", "cpu-first-positive"):
            with self.subTest(fixture=fixture):
                self.commands.clear()
                options = self.go_profile_options(fixture)
                self.b.main(["--no-pgo", "--no-thinlto", *options, "--test"])
                go_commands = [cmd for cmd, _ in self.commands
                               if Path(cmd[0]).name == "go" and cmd[1] in ("build", "test")]
                self.assertIn("-pgo=off", go_commands[0])
                for command in go_commands[1:]:
                    self.assertIn("-pgo=" + str(self.go_profile.resolve()), command)
                self.assertTrue((self.root / "bin/server").is_file())


    def test_go_profile_changed_during_link_or_tests_cannot_publish(self):
        for stage in ("link", "test"):
            with self.subTest(stage=stage):
                options = self.go_profile_options()
                self.go_profile_change_at = stage
                with self.assertRaisesRegex(ValueError, "Go PGO profile.*changed|Go PGO profile.*mismatch"):
                    self.b.main(["--no-pgo", "--no-thinlto", *options, "--test"])
                for name in ("bin/server", "bin/server.tmp", "build/build-receipt.json"):
                    self.assertFalse((self.root / name).exists(), name)


    def test_go_profile_does_not_disable_strict_sqlite_diagnostics(self):
        self.prepare_profile()
        options = self.go_profile_options()
        self.compile_warning = "error: profile-instr-out-of-date"
        with self.assertRaisesRegex(ValueError, "profile-instr-out-of-date"):
            self.b.main(["--pgo", "--thinlto", *options])
        self.assertFalse((self.root / "bin/server").exists())


    def test_go_profile_test_failure_cannot_publish(self):
        options = self.go_profile_options()
        self.tests_fail = True
        with self.assertRaisesRegex(ValueError, "command failed"):
            self.b.main(["--no-pgo", "--no-thinlto", *options, "--test"])
        self.assertFalse((self.root / "bin/server").exists())
        self.assertFalse((self.root / "build/build-receipt.json").exists())



if __name__ == "__main__":
    unittest.main(verbosity=2)
