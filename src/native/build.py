"""Build the JSON extension against pinned SQLite headers, without linking another SQLite."""

import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = "sqlite-amalgamation-3530100"
SHA256 = "36ad6e7f38540a3b21a2ac36340833f0a9e426bc1c752751c3ba669466827eae"


def main() -> None:
    cache = ROOT / ".native"
    cache.mkdir(exist_ok=True)
    archive = cache / f"{ARCHIVE}.zip"
    if not archive.exists():
        with urllib.request.urlopen(
            f"https://www.sqlite.org/2026/{ARCHIVE}.zip", timeout=60
        ) as source:
            archive.write_bytes(source.read())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError("SQLite header archive checksum mismatch")
    with zipfile.ZipFile(archive) as source:
        for name in ("sqlite3.h", "sqlite3ext.h"):
            (cache / name).write_bytes(source.read(f"{ARCHIVE}/{name}"))
    compiler = shutil.which(os.environ.get("CC", "gcc"))
    if compiler is None:
        raise RuntimeError("gcc is required to build native JSON")
    output = ROOT / "api" / ("_json.dll" if sys.platform == "win32" else "_json.so")
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-O3",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-shared",
            *([] if sys.platform == "win32" else ["-fPIC", "-fvisibility=hidden"]),
            "-I",
            str(cache),
            str(ROOT / "native/json.c"),
            "-o",
            str(output),
        ],
        check=True,
    )
    print(f"Built {output.name}")


if __name__ == "__main__":
    main()
