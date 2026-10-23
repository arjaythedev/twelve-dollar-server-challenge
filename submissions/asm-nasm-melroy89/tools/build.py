#!/usr/bin/env python3
"""Pinned source build, release SQLite PGO and reproducible non-PGO diagnostics."""
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

root = pathlib.Path(__file__).resolve().parents[1]
mode = sys.argv[1] if len(sys.argv) > 1 else "release"
io_stats = sys.argv[2:] == ["--io-stats"]
if sys.argv[2:] and not io_stats:
    raise SystemExit("unknown build option")
if mode not in ("release", "diagnostic"):
    raise SystemExit("usage: build.sh [release|diagnostic] [--io-stats]")
for directory in ("deps", "build", "bin"):
    (root / directory).mkdir(exist_ok=True)
archive = root / "deps/sqlite-amalgamation-3540000.zip"
expected = "7b670a62fdfbd672b75fef004cb703c8a3e87d3a5cc7d675b4a08337004a2d93"
if not archive.exists():
    urllib.request.urlretrieve("https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip", archive)
if hashlib.sha3_256(archive.read_bytes()).hexdigest() != expected:
    raise SystemExit("SQLite archive SHA3-256 mismatch")
with zipfile.ZipFile(archive) as z:
    for name in ("sqlite3.c", "sqlite3.h"):
        content = z.read("sqlite-amalgamation-3540000/" + name)
        path = root / "deps" / name
        if not path.exists() or path.read_bytes() != content:
            path.write_bytes(content)

def run(args):
    subprocess.run([str(x) for x in args], cwd=root, check=True)

flags = ["-O3", "-march=native", "-flto", "-fno-plt", "-g",
         "-DSQLITE_THREADSAFE=0", "-DSQLITE_DEFAULT_MEMSTATUS=0", "-DSQLITE_DQS=0",
         "-DSQLITE_OMIT_LOAD_EXTENSION", "-DSQLITE_OMIT_DEPRECATED",
         "-DSQLITE_OMIT_SHARED_CACHE", "-DSQLITE_OMIT_PROGRESS_CALLBACK", "-DHAVE_FDATASYNC=1"]
compiler = subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0]
identity = hashlib.sha256((root / "deps/sqlite3.c").read_bytes() +
                         (root / "src/train.asm").read_bytes() + (root / "src/train-sql.inc").read_bytes() +
                         (root / "src/sql.inc").read_bytes() +
                         json.dumps([mode, compiler, flags]).encode()).hexdigest()
obj = root / ("build/sqlite-" + mode + ".o")
stamp = root / ("build/sqlite-" + mode + ".stamp")
for name in ("crypto", "json", "server"):
    run(["nasm", *(["-DIO_DIAG=1"] if io_stats and name == "server" else []), "-f", "elf64", "-g", "-F", "dwarf", "-Isrc/", "src/" + name + ".asm", "-o", "build/" + name + ".o"])
if not obj.exists() or not stamp.exists() or stamp.read_text() != identity:
    if mode == "release":
        profile = root / "build/profile"
        if profile.exists():
            shutil.rmtree(profile)
        profile.mkdir()
        run(["nasm", "-f", "elf64", "-g", "-F", "dwarf", "-Isrc/", "src/train.asm", "-o", "build/train.o"])
        generate = "-fprofile-generate=" + str(profile)
        run(["gcc", *flags, generate, "-c", "deps/sqlite3.c", "-o", obj])
        run(["gcc", "-no-pie", "-flto", generate, "build/train.o", obj, "-lm", "-o", "build/train"])
        with tempfile.TemporaryDirectory(prefix="synthetic-", dir=root / "build") as tmp:
            run(["build/train", pathlib.Path(tmp) / "training.db"])
        assert list(profile.rglob("*.gcda")), "PGO training produced no profiles"
        run(["gcc", *flags, "-fprofile-use=" + str(profile), "-fprofile-correction",
             "-fprofile-partial-training", "-Werror=missing-profile", "-c", "deps/sqlite3.c", "-o", obj])
    else:
        run(["gcc", *flags, "-c", "deps/sqlite3.c", "-o", obj])
    stamp.write_text(identity)
run(["gcc", "-no-pie", "-flto", "build/server.o", "build/json.o", "build/crypto.o", obj, "-lm", "-o", "bin/server"])
metadata = {"io_stats": io_stats, "sqlite": "3.54.0", "archive_sha3_256": expected, "mode": mode, "compiler": compiler,
            "flags": flags, "sqlite_build_identity": identity,
            "source_sha256": {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in sorted((root / "src").glob("*"))},
            "binary_sha256": hashlib.sha256((root / "bin/server").read_bytes()).hexdigest()}
(root / "build/identity.json").write_text(json.dumps(metadata, indent=2) + "\n")
print(json.dumps({k: metadata[k] for k in ("sqlite", "mode", "compiler", "binary_sha256")}))
