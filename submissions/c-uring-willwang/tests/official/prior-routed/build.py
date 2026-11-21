"""Restore the seven immutable router artifacts in a caller-selected working directory."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
PUBLISHED = {
    "run.py": "d9a26d6523490324dc22249c0f888fe01b4b6173ca57d1b3ae0ed5c25af9316d",
    "veth/placement.c": "ac5f21564bb83acc21d094515682b090d3e820b1b5e847b3e2039f4481bf336a",
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(checkout, work):
    official = checkout.resolve() / "submissions/c-uring-willwang/tests/official"
    for name, expected in PUBLISHED.items():
        if sha(official / name) != expected:
            raise ValueError(f"PR403 baseline source differs: {name}")
    expected = json.loads((HERE / "source/hashes.json").read_text())
    work.mkdir(exist_ok=True, parents=True)
    dest = work / "recovered/router"
    if not dest.exists():
        dest.parent.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=dest.parent) as td:
            tmp = Path(td) / "router"
            tmp.mkdir()
            for name in [
                "icmp.py",
                "launch.py",
                "private_net.py",
                "calibrate.py",
                "run-native",
            ]:
                shutil.copyfile(HERE / "source" / name, tmp / name)
            (tmp / "run-native").chmod(0o755)
            subprocess.run(
                [
                    "patch",
                    "--silent",
                    "--output",
                    tmp / "eth_run.py",
                    official / "run.py",
                    HERE / "source/eth_run.patch",
                ],
                check=True,
            )
            subprocess.run(
                [
                    "gcc",
                    "-Wall",
                    "-Wextra",
                    "-O2",
                    official / "veth/placement.c",
                    "-o",
                    tmp / "placement",
                ],
                check=True,
            )
            for name, record in expected.items():
                got = sha(tmp / name)
                if got != record["expected"]:
                    raise ValueError(
                        f"Historical digest differs for {name}: {got}. Placement requires the recorded GCC/Ubuntu toolchain."
                    )
            tmp.rename(dest)
    for name, record in expected.items():
        if sha(dest / name) != record["expected"]:
            raise ValueError(f"Existing artifact differs: {name}")
    shutil.copyfile(HERE / "source/hashes.json", dest.parent / "proof.json")
    return {
        "work": str(work),
        "source_count": len(expected),
        "original_sha256": {n: v["expected"] for n, v in expected.items()},
    }


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--checkout",
        type=Path,
        required=True,
        help="challenge checkout containing PR403 test inputs",
    )
    p.add_argument(
        "--work",
        type=Path,
        required=True,
        help="writable scratch path for the recovered seven artifacts",
    )
    a = p.parse_args()
    print(json.dumps(build(a.checkout, a.work.resolve()), indent=2))
