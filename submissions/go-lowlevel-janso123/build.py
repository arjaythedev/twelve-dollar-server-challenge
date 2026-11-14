#!/usr/bin/env python3
"""Linux/amd64 builder: Go PGO, SQLite PGO and C/cgo ThinLTO enabled by default.

Profile manifest schema 1 (profiles/manifest.json):
  profile_contract(pins, sqlite_flags), plus
  raw: {file: "sqlite.profraw", sha256: <hex SHA256>, size: <positive integer>}
  provenance: {training_id: <nonempty string>, contract_sha256: <hex SHA256>,
               binary_sha256: <hex SHA256>}
Additional provenance/manifest metadata is permitted. Contract fields must match
exactly. No training, downloading of toolchains, or experiment inputs are used.
ThinLTO is an experimental compile-use change; the validated profile contract
continues to describe the original non-LTO training flags, without rewriting it.
"""
import argparse
import fcntl
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parent
LLVM_VERSION = "23.1.3"
GO_PGO_SHA256 = "cffbedaaa8f7208e6cce79844a858e8ae2f08a3a14079cdc345f0968e63eb6ff"
MODULE = "github.com/mattn/go-sqlite3"
MODULE_VERSION = "v1.14.52"
TAGS = "cgo_sqlite,libsqlite3,sqlite_serialize"
COMMON = ["--no-default-config", "-O2", "-fPIC", "-pthread", "-fno-common", "-Werror"]
GENERATE = ["-fprofile-generate", "-fprofile-update=atomic"]
STRICT_USE = ["-Werror=profile-instr-unprofiled", "-Werror=profile-instr-out-of-date",
              "-Werror=backend-plugin", "-mllvm", "-pgo-warn-missing-function"]
THIN_COMPILE = ["-flto=thin"]
THIN_LINK = ["-flto=thin", "-Wl,--thinlto-jobs=1", "-Wl,--lto-O2"]
SOURCE_FILES = {"sqlite3-binding.c", "sqlite3-binding.h", "sqlite3ext.h",
                "sqlite3.go", "sqlite3_other.go"}
AMBIENT = """CFLAGS CPPFLAGS CXXFLAGS LDFLAGS CGO_CFLAGS CGO_CPPFLAGS CGO_CXXFLAGS
CGO_FFLAGS CGO_LDFLAGS CGO_CFLAGS_ALLOW CGO_LDFLAGS_ALLOW CGO_CFLAGS_DISALLOW
CGO_LDFLAGS_DISALLOW GOFLAGS GOEXPERIMENT CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH
LIBRARY_PATH COMPILER_PATH GCC_EXEC_PREFIX LLVM_PROFILE_FILE GOAL_LLVM_PROFILE
GOAL_SQLITE_GCDA GCOV_PREFIX GCOV_PREFIX_STRIP LD_PRELOAD LD_LIBRARY_PATH
CCC_OVERRIDE_OPTIONS CLANG_CONFIG_FILE_SYSTEM_DIR CLANG_CONFIG_FILE_USER_DIR
SQLITE_LTO GOCACHEPROG GOAMD64""".split()


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path, value):
    temp = path.with_name(path.name + ".tmp")
    temp.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n")
    temp.replace(path)


def source_identity():
    """Hash local build inputs, including cfeed/cgo and the Go module locks."""
    paths = [path for directory in (ROOT / "app", ROOT / "v3probe")
             for path in directory.rglob("*") if path.is_file()
             and not set(path.relative_to(directory).parts) & {"bin", "build", "__pycache__"}]
    paths += [ROOT / name for name in ("build.py", "build.sh", "sqlite-source.sha256")
              if (ROOT / name).is_file()]
    return {str(path.relative_to(ROOT)): sha(path) for path in sorted(paths)}


def arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0], allow_abbrev=False)
    pgo = parser.add_mutually_exclusive_group()
    pgo.add_argument("--pgo", action="store_true", default=True,
                     help="enable SQLite PGO with the included profile (default); requires x86-64-v3")
    pgo.add_argument("--no-pgo", dest="pgo", action="store_false",
                     help="disable SQLite PGO; probe v3 with v1 fallback")
    thinlto = parser.add_mutually_exclusive_group()
    thinlto.add_argument("--thinlto", action="store_true", default=True,
                         help="enable SQLite/cfeed/cgo ThinLTO with one backend job (default)")
    thinlto.add_argument("--no-thinlto", dest="thinlto", action="store_false",
                         help="disable ThinLTO independently of SQLite PGO")
    parser.add_argument("--test", action="store_true", help="run Go tests using the final build flags before publishing bin/server")
    parser.add_argument("--baseline", action="store_true",
                        help="disable Go and SQLite PGO and force x86-64-v1; rejects custom Go profiles; ThinLTO stays enabled unless --no-thinlto")
    go_pgo = parser.add_mutually_exclusive_group()
    go_pgo.add_argument("--go-pgo", metavar="PATH", help="replace bundled profiles/go.pprof for app build/tests; requires --go-pgo-sha256")
    go_pgo.add_argument("--no-go-pgo", action="store_true", help="disable Go PGO independently of SQLite PGO and ThinLTO")
    parser.add_argument("--go-pgo-sha256", metavar="HASH", help="required lowercase SHA-256 of custom --go-pgo; bundled profile uses its pinned digest by default")
    args = parser.parse_args(argv)
    if args.no_go_pgo and args.go_pgo_sha256 is not None:
        parser.error("--no-go-pgo cannot be combined with --go-pgo-sha256")
    if (args.go_pgo is None) != (args.go_pgo_sha256 is None):
        parser.error("--go-pgo and --go-pgo-sha256 must be provided together")
    if args.go_pgo_sha256 is not None and not re.fullmatch(r"[0-9a-f]{64}", args.go_pgo_sha256):
        parser.error("--go-pgo-sha256 must be a lowercase 64-digit SHA-256 hash")
    if args.baseline:
        if "--pgo" in argv:
            parser.error("--pgo requires x86-64-v3; --baseline cannot be combined with --pgo")
        if args.go_pgo is not None:
            parser.error("--baseline cannot be combined with a custom --go-pgo profile")
        args.pgo = False
    if args.go_pgo is None and not args.no_go_pgo and not args.baseline:
        args.go_pgo = str(ROOT / "profiles/go.pprof")
        args.go_pgo_sha256 = GO_PGO_SHA256
    return args


def validate_environment(env):
    for name in AMBIENT:
        require(not env.get(name), f"ambient build flags are forbidden: {name}; use build.sh options")
    for name, expected in {"GOOS": "linux", "GOARCH": "amd64", "CGO_ENABLED": "1",
                           "GOTOOLCHAIN": "local", "GO_PGO": "off", "OPT_LEVEL": "2"}.items():
        require(not env.get(name) or env[name] == expected, f"{name} must be {expected}")
    for name in ("CC", "AR", "LD", "PROFDATA"):
        require(not env.get(name), f"{name} is controlled by LLVM_BIN, not an independent override")


def source_pins(path):
    pins = {}
    for line in path.read_text().splitlines():
        fields = line.split()
        require(len(fields) == 2 and re.fullmatch(r"[0-9a-f]{64}", fields[0]),
                "invalid sqlite-source.sha256 entry")
        digest, name = fields
        require(name in SOURCE_FILES and name not in pins, f"unexpected/duplicate SQLite source pin: {name}")
        pins[name] = digest
    require(set(pins) == SOURCE_FILES, "sqlite-source.sha256 must pin all five SQLite source inputs")
    return pins


def prepare_sqlite(module, work, pins):
    for name, digest in pins.items():
        require((module / name).is_file() and sha(module / name) == digest,
                f"pinned SQLite source mismatch: {name}")
    definitions = [flag for line in (module / "sqlite3.go").read_text().splitlines()
                   if line.startswith("#cgo CFLAGS: ")
                   for flag in shlex.split(line[len("#cgo CFLAGS: "):])]
    require("-DSQLITE_THREADSAFE=1" in definitions, "SQLITE_THREADSAFE=1 required")
    # Exactly the tested mattn wrapper removal: never use the host SQLite headers.
    shutil.copyfile(module / "sqlite3-binding.c", work / "sqlite3.c")
    for name, dest in (("sqlite3-binding.h", "sqlite3.h"), ("sqlite3ext.h", "sqlite3ext.h")):
        data = (module / name).read_bytes().splitlines(keepends=True)
        require(data and data[0].strip() == b"#ifndef USE_LIBSQLITE3", f"pinned header wrapper missing: {name}")
        data = data[1:]
        tail = next((i for i, line in enumerate(data) if line.startswith(b"#else // USE_LIBSQLITE3")), None)
        require(tail is not None, f"pinned header wrapper missing: {name}")
        (work / dest).write_bytes(b"".join(data[:tail]))
    return definitions


def profile_contract(pins, sqlite_flags):
    """Exact portable subset of the tested training contract, for the parent."""
    return {"schema": 1, "llvm_version": LLVM_VERSION, "target": "linux/amd64", "goamd64": "v3",
            "sqlite_module": {"path": MODULE, "version": MODULE_VERSION},
            "sqlite_sources": pins, "sqlite_flags": sqlite_flags, "generate_flags": GENERATE,
            "go": {"pgo": "off", "tags": TAGS}}


def validate_profile(directory, pins, sqlite_flags):
    path = directory / "manifest.json"
    require(path.is_file() and not path.is_symlink(), f"missing or symlink profile manifest: {path}")
    manifest = json.loads(path.read_text())
    require(isinstance(manifest, dict), "profile manifest must be an object")
    for key, value in profile_contract(pins, sqlite_flags).items():
        require(type(manifest.get(key)) is type(value) and manifest[key] == value,
                f"profile manifest mismatch: {key}; expected {value!r}")
    raw = directory / "sqlite.profraw"
    require(raw.is_file() and not raw.is_symlink() and raw.stat().st_size > 0,
            f"missing, empty, or symlink raw profile: {raw}")
    expected_raw = {"file": "sqlite.profraw", "sha256": sha(raw), "size": raw.stat().st_size}
    require(isinstance(manifest.get("raw"), dict) and manifest["raw"] == expected_raw
            and type(manifest["raw"].get("size")) is int, "profile manifest mismatch: raw file/hash/size")
    provenance = manifest.get("provenance")
    require(isinstance(provenance, dict), "profile manifest missing provenance")
    require(isinstance(provenance.get("training_id"), str) and provenance["training_id"].strip(),
            "profile provenance missing training_id")
    for key in ("contract_sha256", "binary_sha256"):
        require(isinstance(provenance.get(key), str) and re.fullmatch(r"[0-9a-f]{64}", provenance[key]),
                f"profile provenance invalid {key}")
    require(not Path(str(raw) + ".lock").exists(), "raw profile export lock exists; export is unfinished")
    return manifest


def validate_go_profile(path, expected_sha256):
    """Bind a bundled or custom Go profile to its pinned/caller-supplied digest."""
    path = Path(path)
    require(path.is_file() and not path.is_symlink() and path.stat().st_size > 0,
            f"missing, empty, or symlink Go PGO profile: {path}")
    resolved = path.resolve()
    require(sha(resolved) == expected_sha256, f"Go PGO profile SHA-256 mismatch: {resolved}")
    return {"path": str(resolved), "sha256": expected_sha256}


def profile_varint(data, offset):
    """Read one bounded protobuf uint64 (also the wire encoding of int64)."""
    value = 0
    for shift in range(0, 70, 7):
        require(offset < len(data), "invalid Go PGO profile: truncated varint")
        byte = data[offset]
        offset += 1
        require(shift < 63 or byte < 2, "invalid Go PGO profile: varint overflow")
        value |= (byte & 127) << shift
        if byte < 128:
            return value, offset
    raise ValueError("invalid Go PGO profile: varint overflow")


def profile_fields(data):
    """Walk pprof protobuf fields, checking lengths and skipping unknown fields."""
    data = memoryview(data)
    offset = 0
    while offset < len(data):
        tag, offset = profile_varint(data, offset)
        number, wire = tag >> 3, tag & 7
        require(0 < number < 1 << 29, "invalid Go PGO profile: field number")
        if wire == 0:
            value, offset = profile_varint(data, offset)
        else:
            require(wire in (1, 2, 5), "invalid Go PGO profile: wire type")
            if wire == 2:
                size, offset = profile_varint(data, offset)
            else:
                size = 8 if wire == 1 else 4
            end = offset + size
            require(end <= len(data), "invalid Go PGO profile: truncated field")
            value, offset = data[offset:end], end
        yield number, wire, value


def validate_go_cpu_samples(path):
    """Check CPU and compiler-selected signed weight after pprof structure checks.

    Read only sample_type, sample.value and string_table from profile.proto.
    Display totals round/scale units and pprof succeeds even for zero CPU weight.
    Both gzip/plain protobufs and packed/unpacked repeated values are supported;
    Go's pprof remains responsible for stacks, references and other semantics.
    """
    data = Path(path).read_bytes()
    if data.startswith(b"\x1f\x8b"):
        try:
            data = gzip.decompress(data)
        except (OSError, EOFError, zlib.error) as error:
            raise ValueError("invalid Go PGO profile: gzip data") from error
    types, samples, strings = [], [], []
    for number, wire, value in profile_fields(data):
        if number in (1, 2, 6):
            require(wire == 2, "invalid Go PGO profile: sample/string field type")
            if number == 1:
                indices = {}
                for field, kind, index in profile_fields(value):
                    if field in (1, 2):
                        require(kind == 0, "invalid Go PGO profile: sample type index")
                        indices[field] = index
                types.append((indices.get(1, 0), indices.get(2, 0)))
            elif number == 2:
                samples.append(value)
            else:
                strings.append(bytes(value))
    require(strings and strings[0] == b"", "invalid Go PGO profile: string table")
    require(all(name < len(strings) and unit < len(strings) for name, unit in types),
            "invalid Go PGO profile: sample type string index")
    cpu = next((i for i, (name, _) in enumerate(types) if strings[name] == b"cpu"), None)
    require(cpu is not None and strings[types[cpu][1]] == b"nanoseconds",
            "Go PGO profile must contain cpu/nanoseconds samples")
    # Go 1.27.2 cmd/internal/pgo/pprof.go:FromPProf selects the FIRST
    # samples/count OR cpu/nanoseconds column, regardless of the default index.
    # A positive CPU column alone can therefore hide an unused, zero profile.
    selected = next(i for i, (name, unit) in enumerate(types)
                    if (strings[name], strings[unit]) in
                    ((b"samples", b"count"), (b"cpu", b"nanoseconds")))
    total, selected_total = 0, 0
    for sample in samples:
        values = []
        for field, wire, value in profile_fields(sample):
            if field == 2:
                require(wire in (0, 2), "invalid Go PGO profile: sample value type")
                if wire == 0:
                    values.append(value)
                else:
                    offset = 0
                    while offset < len(value):
                        item, offset = profile_varint(value, offset)
                        values.append(item)
        require(len(values) == len(types), "invalid Go PGO profile: sample value count")
        weight = values[cpu]
        total += weight if weight < 1 << 63 else weight - (1 << 64)
        weight = values[selected]
        selected_total += weight if weight < 1 << 63 else weight - (1 << 64)
    require(total > 0, "Go PGO profile must have positive CPU weight")
    selected_type = "/".join(strings[index].decode() for index in types[selected])
    require(selected_total > 0,
            f"Go PGO profile must have positive compiler-selected weight ({selected_type}, column {selected})")



def profile_records(shown):
    """Read LLVM's pinned-version dump to verify remapping preserves counters."""
    records = {}
    pattern = r"^  (\S+):\n    Hash: (0x[0-9a-f]+)\n    Counters: (\d+)\n    Block counts: (\[[^\n]*\])"
    for name, function_hash, count, blocks in re.findall(pattern, shown, re.M):
        key = (name, function_hash)
        require(key not in records, "duplicate profile function/hash")
        values = json.loads(blocks)
        require(len(values) == int(count), "profile counter count mismatch")
        records[key] = values
    require(records and len(records) == len(re.findall(r"^  \S+:$", shown, re.M)),
            "unexpected LLVM profile record format")
    return records


def relocated_records(records, old_source, new_source):
    require(isinstance(old_source, str) and old_source.startswith("/")
            and old_source.endswith("/sqlite3.c") and not any(c.isspace() for c in old_source),
            "profile source_filename must identify the training SQLite source")
    prefix = old_source + ";"
    pairs, relocated = {}, {}
    for (name, function_hash), counts in records.items():
        if ";" in name:
            require(name.startswith(prefix), "unexpected static-function source in profile")
            target = str(new_source) + ";" + name[len(prefix):]
            pairs[name] = target
        else:
            target = name
        key = (target, function_hash)
        require(key not in relocated, "profile relocation collides with another function")
        relocated[key] = counts
    require(pairs, "profile contains no SQLite static functions to relocate")
    return pairs, relocated


def select_cpu(supports_v3, *, pgo, baseline):
    require(not pgo or supports_v3, "SQLite PGO requires x86-64-v3 hardware; use --no-pgo to disable SQLite PGO; no PGO fallback is permitted")
    return "v1" if baseline or not supports_v3 else "v3"


class Driver:
    def __init__(self, work, args):
        self.work, self.args = work, args
        llvm = Path(os.environ.get("LLVM_BIN", "/opt/llvm-23.1.3/bin")).resolve()
        self.tools = {"CC": llvm / "clang", "AR": llvm / "llvm-ar", "LD": llvm / "ld.lld"}
        if args.pgo:
            self.tools["PROFDATA"] = llvm / "llvm-profdata"
        self.go = Path(os.environ.get("GO", "/usr/local/go/bin/go")).resolve()
        # External linker arguments and cgo's flag strings cannot safely encode
        # arbitrary paths. Reject them explicitly instead of relying on quoting.
        for path in (ROOT, work, self.go, *self.tools.values()):
            require(not any(c.isspace() or c in "'\"\\%" for c in str(path)),
                    f"build/tool paths must not contain whitespace, quotes, backslashes, or %: {path}")
        self.env = dict(os.environ)
        self.env.pop("GOROOT", None)  # let the selected Go installation locate its root
        self.env.update({"LC_ALL": "C", "CC": str(self.tools["CC"]), "AR": str(self.tools["AR"]),
                         "CGO_ENABLED": "1", "GOTOOLCHAIN": "local", "GOENV": "off", "GOWORK": "off",
                         "GOOS": "linux", "GOARCH": "amd64", "GOFLAGS": "", "GOEXPERIMENT": "",
                         "GOTELEMETRY": "off"})
        # Use the ordinary Go caches and module proxy/checksum settings. A clean
        # build may fetch the versions pinned by go.mod/go.sum; never install tools.
        self.definitions = []
        self.pins = source_pins(ROOT / "sqlite-source.sha256")
        self.profile = None
        self.go_profile = (validate_go_profile(args.go_pgo, args.go_pgo_sha256)
                           if args.go_pgo is not None else None)
        self.arch = "v1"
        self.common = COMMON + ["-march=x86-64"]
        self.lto_compile = THIN_COMPILE if args.thinlto else []
        self.lto_link = THIN_LINK if args.thinlto else []
        self.link = []
        self.compiler_sha = None

    def run(self, args, *, cwd=None, input=None, check=True):
        command = [str(arg) for arg in args]
        result = subprocess.run(command, cwd=cwd or ROOT / "app", env=self.env,
                                input=input, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        with (self.work / "build.log").open("a") as stream:
            stream.write("$ " + shlex.join(command) + "\n" + result.stdout)
        require(not check or result.returncode == 0,
                f"command failed ({result.returncode}): {shlex.join(command)}\n{result.stdout[-6000:]}")
        return result

    def output(self, args, **kwargs):
        return self.run(args, **kwargs).stdout

    def prepare(self):
        for name, path in {"GO": self.go, **self.tools}.items():
            require(path.is_file() and os.access(path, os.X_OK),
                    f"missing executable: {path}; set {'GO' if name == 'GO' else 'LLVM_BIN'} to an installed pinned toolchain")
            version = self.output([path, "version" if name == "GO" else "--version"]).strip()
            if name == "GO":
                require(version == "go version go1.27.2 linux/amd64", "Go 1.27.2 linux/amd64 required")
            else:
                require(re.search(r"\b23\.1\.3\b", version), f"{name}: LLVM {LLVM_VERSION} required; got {version}")
        for tool in ("readelf", "nm"):
            require(shutil.which(tool), f"required Linux binutils tool is missing: {tool}")
        macros = self.output([self.tools["CC"], "--no-default-config", "-dM", "-E", "-"], input="")
        for name, value in (("__clang_major__", "23"), ("__clang_minor__", "1"),
                            ("__clang_patchlevel__", "3"), ("__linux__", "1"), ("__x86_64__", "1")):
            require(f"#define {name} {value}\n" in macros, f"incorrect compiler target/version: {name}")
        self.compiler_sha = sha(self.tools["CC"])
        if self.go_profile is not None:
            # Validate structure without symbolization, remote lookups, a reference
            # binary, or a large raw/proto dump. Units/weight need a separate check;
            # the parent still owns collection and workload representativeness.
            self.run([self.go, "tool", "pprof", "-top", "-nodecount=1",
                      "-sample_index=cpu", "-symbolize=none", self.go_profile["path"]])
            validate_go_cpu_samples(self.go_profile["path"])
        # Download only the locked modules when needed, without modifying go.mod.
        self.run([self.go, "mod", "download"])
        self.run([self.go, "mod", "verify"])
        module = json.loads(self.output([self.go, "list", "-m", "-json", MODULE]))
        require(module.get("Version") == MODULE_VERSION and not module.get("Replace"),
                f"unmodified {MODULE}@{MODULE_VERSION} required")
        self.module = Path(module["Dir"])
        self.definitions = prepare_sqlite(self.module, self.work, self.pins)
        if self.args.pgo:
            self.profile = validate_profile(ROOT / "profiles", self.pins,
                                            COMMON + ["-march=x86-64-v3"] + self.definitions)

    def ldflags(self, *, link_map=False):
        flags = self.link.copy()
        if link_map:
            flags += [f"-Wl,-Map={self.work / 'server.link.map'}"]
            if self.args.thinlto:
                # LTO maps name synthesized objects (lto.tmp). Record archive
                # extraction before LTO so the selected member remains provable.
                flags += [f"-Wl,--why-extract={self.work / 'server.why-extract.tsv'}"]
        return f"-linkmode=external -extld={self.tools['CC']} -extldflags '{' '.join(flags)}'"

    def go_options(self, *, link_map=False, cpu_probe=False):
        profile = self.go_profile["path"] if self.go_profile is not None and not cpu_probe else "off"
        return ["-p=1", "-pgo=" + profile, "-mod=readonly", "-trimpath", "-buildvcs=false",
                "-tags=" + TAGS, "-ldflags=" + self.ldflags(link_map=link_map)]

    def probe_v3(self):
        self.env["GOAMD64"] = "v3"
        self.env["CGO_CFLAGS"] = " ".join(COMMON + ["-march=x86-64-v3"] + self.lto_compile)
        self.env["CGO_LDFLAGS"] = " ".join(self.link)
        probe = self.work / "v3probe"
        # Compiler/linker failures are build errors. Only an executed CPU probe
        # can select v1; do not disguise a broken toolchain as older hardware.
        self.run([self.go, "build", *self.go_options(cpu_probe=True), "-o", probe, "."], cwd=ROOT / "v3probe")
        result = self.run([probe], check=False)
        if result.returncode == 0:
            return True
        require(result.returncode in (1, -4) or
                (result.returncode == 2 and "GOAMD64" in result.stdout),
                f"CPU probe failed unexpectedly ({result.returncode}): {result.stdout}")
        return False

    def configure(self):
        # Go's default exe contains non-PIC Go objects. Request non-PIE in
        # both -extldflags and CGO_LDFLAGS (including intermediate cgo links),
        # identically for control and treatment, rather than relying on Go's
        # optional -no-pie support probe. Go 1.27.2's trimLinkerArgv retains
        # ThinLTO flags but drops --ld-path, so that probe can use another ld.
        self.link = ["--no-default-config", "-O2", "-no-pie", "-march=x86-64-v3", f"--ld-path={self.tools['LD']}"] + self.lto_link
        supported = False if self.args.baseline else self.probe_v3()
        self.arch = select_cpu(supported, pgo=self.args.pgo, baseline=self.args.baseline)
        march = "-march=x86-64-v3" if self.arch == "v3" else "-march=x86-64"
        # Keep this non-LTO flag list exact: profile validation and relocation
        # bind it to the original training contract at every existing checkpoint.
        self.common = COMMON + [march] + self.definitions
        self.link = ["--no-default-config", "-O2", "-no-pie", march, f"--ld-path={self.tools['LD']}"] + self.lto_link
        self.env["GOAMD64"] = self.arch
        print(f"build: Linux/amd64 {self.arch}; SQLite PGO={'on' if self.args.pgo else 'off'}; Go PGO={self.go_profile['path'] if self.go_profile else 'off'}; "
              f"ThinLTO={'experimental (backend jobs=1)' if self.args.thinlto else 'off'}", flush=True)

    def merge_profile(self):
        merged = self.work / "sqlite.profdata"
        temp = merged.with_suffix(".profdata.tmp")
        temp.unlink(missing_ok=True)
        merged.unlink(missing_ok=True)
        raw = ROOT / "profiles/sqlite.profraw"
        tool = self.tools["PROFDATA"]
        original = self.output([tool, "show", "--all-functions", "--counts", raw])
        pairs, expected = relocated_records(profile_records(original),
                                             self.profile.get("source_filename"), self.work / "sqlite3.c")
        remapping = self.work / "profile-remapping.txt"
        remapping.write_text("".join(f"{name} {target}\n" for name, target in sorted(pairs.items())))
        self.run([tool, "merge", "--failure-mode=any", f"--remapping-file={remapping}", "-o", temp, raw])
        shown = self.output([tool, "show", "--all-functions", "--counts", temp])
        require(profile_records(shown) == expected,
                "profile relocation changed function hashes or execution counters")
        require("Instrumentation level: IR" in shown, "profile must contain LLVM IR instrumentation data")
        count = re.search(r"Maximum function count:\s*(\d+)", shown)
        require(count and int(count[1]) > 0, "profile contains no executed function counters")
        require("sqlite3_open_v2" in shown and "sqlite3_step" in shown,
                "profile does not contain expected SQLite functions")
        require(self.profile == validate_profile(ROOT / "profiles", self.pins, self.common),
                "raw profile or manifest changed during merge")
        temp.replace(merged)
        return merged

    def sqlite_archive(self):
        flags = self.common + self.lto_compile
        profile_sha = None
        if self.args.pgo:
            merged = self.merge_profile()
            flags += [f"-fprofile-use={merged}"] + STRICT_USE
            profile_sha = sha(merged)
        identity = {"flags": flags, "sources": self.pins, "compiler_sha256": self.compiler_sha,
                    "profdata_sha256": profile_sha, "archiver_sha256": sha(self.tools["AR"]),
                    "link_flags": self.link, "linker_sha256": sha(self.tools["LD"])}
        key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
        obj, archive = self.work / "sqlite3.o", self.work / "libsqlite3.a"
        receipt = self.work / "sqlite-build.json"
        cached = json.loads(receipt.read_text()) if receipt.is_file() else {}
        if not (cached.get("key") == key and obj.is_file() and archive.is_file()
                and cached.get("object_sha256") == sha(obj) and cached.get("archive_sha256") == sha(archive)):
            receipt.unlink(missing_ok=True)
            archive.unlink(missing_ok=True)  # rcs must not retain stale members
            self.run([self.tools["CC"], *flags, "-c", self.work / "sqlite3.c", "-o", obj])
            if self.args.thinlto:
                # An LTO object is LLVM bitcode, not ELF. Validate its magic and
                # parse it with the selected Clang, without materializing huge IR
                # dumps or compiling another native object. Final ELF checks below
                # still reject training sections/runtime after LTO code generation.
                with obj.open("rb") as stream:
                    require(stream.read(4) == b"BC\xc0\xde", "ThinLTO SQLite object is not LLVM bitcode")
                self.run([self.tools["CC"], "--no-default-config", "-Werror", "-x", "ir",
                          "-S", "-emit-llvm", obj, "-o", "/dev/null"])
            else:
                sections = self.output(["readelf", "-SW", obj])
                require("__llvm_prf" not in sections and "__llvm_cov" not in sections,
                        "LLVM training counters leaked into SQLite object")
            self.run([self.tools["AR"], "rcs", archive, obj])
            require(self.output([self.tools["AR"], "t", archive]).strip() == "sqlite3.o",
                    "unexpected SQLite archive members")
            write_json(receipt, {"key": key, "identity": identity,
                                 "object_sha256": sha(obj), "archive_sha256": sha(archive)})
        # Go's cgo cache does not track the contents of external static archives.
        # This unused define changes its cache key whenever our archive changes,
        # without forcing a rebuild of Go's entire standard library (-a).
        self.env["CGO_CFLAGS"] = " ".join(self.common + self.lto_compile + [f"-I{self.work}", f"-DGO_SQLITE_BUILD_ID=0x{key[:16]}"])
        self.env["CGO_LDFLAGS"] = " ".join([f"-L{self.work}", str(archive), "-ldl", "-pthread", "-lm", *self.link])
        return archive

    def verify_binary(self, binary, archive):
        elf = self.output(["readelf", "-h", "-d", "-SW", binary])
        symbols = self.output(["nm", "--defined-only", binary])
        require("Advanced Micro Devices X86-64" in elf, "binary is not Linux x86_64")
        require(not re.search(r"NEEDED.*sqlite", elf, re.I), "unexpected dynamic SQLite dependency")
        require("__llvm_prf" not in elf and "__llvm_cov" not in elf,
                "LLVM training counters leaked into final ELF")
        for symbol in ("sqlite3_libversion", "sqlite3_threadsafe"):
            require(re.search(rf" [Tt] {symbol}$", symbols, re.M), f"missing static SQLite symbol: {symbol}")
        for symbol in ("__llvm_profile", "__gcov", "goal_llvm_profile_export"):
            require(symbol not in symbols, f"training runtime/hook leaked into final binary: {symbol}")
        if self.args.thinlto:
            proof = self.work / "server.why-extract.tsv"
            rows = [line.split("\t") for line in proof.read_text().splitlines()] if proof.is_file() else []
            require(any(len(row) == 3 and row[1] == f"{archive}(sqlite3.o)"
                        and row[2].startswith("sqlite3_") for row in rows),
                    "selected SQLite archive missing from LLD extraction report")
        else:
            require(f"{archive}(sqlite3.o)" in (self.work / "server.link.map").read_text(),
                    "selected SQLite archive missing from LLD link map")
        self.run([self.go, "version", "-m", binary])

    def build(self, temp):
        inputs = source_identity()
        self.prepare()
        self.configure()
        archive = self.sqlite_archive()
        (self.work / "server.link.map").unlink(missing_ok=True)
        (self.work / "server.why-extract.tsv").unlink(missing_ok=True)
        self.run([self.go, "build", *self.go_options(link_map=True), "-o", temp, "."])
        self.verify_binary(temp, archive)
        if self.args.test:
            # A separate map-free link keeps the server's archive proof intact.
            self.run([self.go, "test", *self.go_options(), "-count=1", "-timeout=120s", "./..."])
        if self.args.pgo:
            require(self.profile == validate_profile(ROOT / "profiles", self.pins, self.common),
                    "raw profile or manifest changed during build")
        if self.go_profile is not None:
            require(self.go_profile == validate_go_profile(self.args.go_pgo, self.args.go_pgo_sha256),
                    "Go PGO profile path changed during build")
        for name, digest in self.pins.items():
            require(sha(self.module / name) == digest, f"SQLite input changed during build: {name}")
        require(source_identity() == inputs, "local source/build inputs changed during build")
        write_json(self.work / "build-receipt.json", {
            "schema": 1, "mode": ("pgo-thinlto" if self.args.thinlto else "pgo") if self.args.pgo
                                else ("thinlto" if self.args.thinlto else "non-pgo"),
            "experimental_compile_use_change": self.args.thinlto,
            "thinlto_backend_jobs": 1 if self.args.thinlto else None,
            "source_sha256": inputs, "sqlite_sources": self.pins,
            "tools": {name: {"path": str(path), "sha256": sha(path)}
                      for name, path in {"GO": self.go, **self.tools}.items()},
            "training_contract": profile_contract(self.pins, self.common) if self.args.pgo else None,
            "profile_manifest_sha256": sha(ROOT / "profiles/manifest.json") if self.args.pgo else None,
            "raw_profile_sha256": sha(ROOT / "profiles/sqlite.profraw") if self.args.pgo else None,
            "sqlite_build": json.loads((self.work / "sqlite-build.json").read_text()),
            "cgo_cflags": self.env["CGO_CFLAGS"], "cgo_ldflags": self.env["CGO_LDFLAGS"],
            "go_pgo": self.go_profile,
            "go_options": self.go_options(link_map=True), "go_tests": self.args.test,
            "binary_sha256": sha(temp), "archive_sha256": sha(archive),
            "archive_proof": "server.why-extract.tsv" if self.args.thinlto else "server.link.map",
            "build_log_sha256": sha(self.work / "build.log"),
        })


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv in (["--help"], ["-h"]):
        arguments(argv)
        return
    work = ROOT / "build"
    work.mkdir(parents=True, exist_ok=True)
    with (work / "build.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError("another build is running; wait for it to finish") from None
        output = ROOT / "bin/server"
        temp = output.with_name("server.tmp")
        # Any failed request must not leave yesterday's server looking successful.
        output.unlink(missing_ok=True)
        temp.unlink(missing_ok=True)
        (work / "build-receipt.json").unlink(missing_ok=True)
        try:
            args = arguments(argv)
            validate_environment(os.environ)
            require(platform.system() == "Linux" and platform.machine() == "x86_64",
                    "Linux x86_64 required; cross-compilation is not supported")
            output.parent.mkdir(parents=True, exist_ok=True)
            (work / "build.log").write_text("")
            driver = Driver(work, args)
            driver.build(temp)
            temp.replace(output)
            print(f"built: {output}", flush=True)
        finally:
            temp.unlink(missing_ok=True)
            (work / "sqlite.profdata.tmp").unlink(missing_ok=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError) as error:
        sys.exit(f"error: {error}")
