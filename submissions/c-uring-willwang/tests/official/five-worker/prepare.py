"""Reproduce the five-external-worker profile and its exact historical relay ELF."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
MANIFEST_SHA = "79fcebf1e402295ab437ae86dbaa0a63a667d7d5c0f2d1ee18193a27f6bec606"
PRIMARY_SOURCES = {
    "router/assess.py": "467dedd76b453664cf9c45e22013059ebe416128eb356052fb266e71dc4dfd9a",
    "router/run.py": "11c8d5ab51f1109e2aed86b7a07de5d0feeebbd5e45130d3e2c9b525f7bc3179",
    "tap-sharded/run_sharded.py": "ba70cc0b8e0331003bdd11d3c33f23efa3acfc6f6c929c998b2c00fe6222e596",
}


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def source_path(name):
    if name.startswith("five-worker/"):
        return HERE / Path(name).name
    if name == "tap-split/relay.c":
        return HERE / "historical-relay.c"
    if name == "router/scored_common.py":
        return HERE.parent / "router/assess.py"
    return HERE.parent / name


def prepare(destination, archive, cc):
    manifest_file = HERE / "manifest.json"
    if sha(manifest_file) != MANIFEST_SHA:
        raise ValueError("five-worker manifest fingerprint differs")
    manifest = json.loads(manifest_file.read_bytes())
    if destination.exists() or destination.is_symlink():
        raise FileExistsError(f"use a fresh fixture directory: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".five-worker-build-", dir=destination.parent) as temporary:
        staging = Path(temporary)
        fixture = staging / "fixture"
        for name, digest in manifest["files"].items():
            source = source_path(name)
            if sha(source) != PRIMARY_SOURCES.get(name, digest):
                raise ValueError(f"checkout input differs from the five-worker reference: {source}")
            target = fixture / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        subprocess.run(["patch", "--batch", "--forward", "--fuzz=0", "-p1", "-i", str(HERE / "shared-inputs.patch")],
                       cwd=fixture, check=True, stdout=subprocess.DEVNULL)
        for name, digest in manifest["files"].items():
            if sha(fixture / name) != digest:
                raise ValueError(f"materialized source differs: {name}")
        source = fixture / "tap-split/relay.c"
        binary = source.with_name("relay")
        if archive is None:
            # GCC records the original absolute source and build paths in DWARF.
            build = staging / "cc"
            build.mkdir()
            subprocess.run([
                cc, "-O3", "-g", "-Wall", "-Wextra", "-Werror", "-std=gnu11", "-pthread",
                f"-ffile-prefix-map={source.parent}=/tmp/twelve-revisit/tap-sharded/split",
                f"-ffile-prefix-map={build}=/home/dev-user/code/openai",
                str(source), "-o", str(binary),
            ], cwd=build, check=True)
        else:
            if sha(archive) != manifest["relay_sha256"]:
                raise ValueError("supplied relay differs from the measured historical executable")
            shutil.copyfile(archive, binary)
            binary.chmod(0o755)
        if sha(binary) != manifest["relay_sha256"]:
            raise ValueError("the compiler produced a different ELF; use the documented toolchain or --archival-relay")
        shutil.copyfile(manifest_file, fixture / "manifest.json")
        fixture.rename(destination)
    return {"fixture": str(destination), "manifest_sha256": MANIFEST_SHA,
            "verified_source_count": len(manifest["files"]), "relay_sha256": manifest["relay_sha256"]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="fresh <study>/five-worker/fixture directory")
    parser.add_argument("--archival-relay", type=Path, help="already-built historical switch with the exact manifest hash")
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    # Keep an existing final-component symlink visible to the collision guard.
    dest = args.output.parent.resolve() / args.output.name
    print(json.dumps(prepare(dest, args.archival_relay, args.cc), indent=2))
