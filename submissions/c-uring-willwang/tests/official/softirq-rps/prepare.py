"""Prepare the receive-mode experiment from fingerprinted sources in a challenge checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
PIN = "7cf6b912279a283932ba1c3a495acf811c6bb099"
SHARED_SOURCES_SHA256 = "f149b8fb5b6e7ee142ccee09db425a29de181796a3641dd24718409dd527ead4"
OFFICIAL = "submissions/c-uring-willwang/tests/official/"
PRIMARY_SHAPE = "e3d13d7eebac9bec1b2767b1792ae135913c619615abedb29523c7c570349cf7"
MLOAD = "48df1cd6242f55ee5be98238a54fbf7861fcc83c6e3cc91d334b7903d4f208e6"
COMMON = (
    "router/assess.py", "router/launch.py", "router/measure.py", "router/run.py",
    "tap-poll/check_flow.py", "tap-poll/run_tap.py", "tap-poll/tap_napi.py",
    "tap-ring4096/run_buffered.py", "tap-sharded/run_sharded.py", "tap-split/run_split.py",
    "tap-split/relay.c", "threaded-napi/run_threaded.py", "threaded-napi/socket_probe.py",
)
ADDED = ("router/assess_rps.py", "softirq-rps/private_queue.py", "softirq-rps/run_rps.py")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def replace_once(data, original, replacement):
    if data.count(original) != 1:
        raise ValueError(f"expected one occurrence of {original!r} in the reference")
    return data.replace(original, replacement)


def prepare(checkout, work, existing, compiler):
    proof_file = "tap-split/relay-lifecycle-proof.json"
    shared = {name: (checkout / OFFICIAL / name).read_bytes() for name in (*COMMON, proof_file)}
    digests = {name: sha(data) for name, data in shared.items()}
    source_digest = sha(json.dumps(digests, sort_keys=True, separators=(",", ":")).encode())
    if source_digest != SHARED_SOURCES_SHA256:
        raise ValueError(f"shared fixture files do not match reference revision {PIN}")
    proof = json.loads(shared[proof_file])
    if existing and sha(existing.read_bytes()) != proof["scoring_elf_sha256"]:
        raise ValueError("supplied archival relay does not match the original scoring ELF")
    work.mkdir(parents=True, exist_ok=False)
    root = work / "frozen"
    for name in (*COMMON, *ADDED):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        data = shared[name] if name in COMMON else (HERE / path.name).read_bytes()
        if name == "tap-poll/run_tap.py":
            data = replace_once(data, b'HERE / "tap_napi.py"', b'"-I", "-B", HERE.parent / "softirq-rps" / "private_queue.py"')
            data = replace_once(data, b'topology[server]["threaded_napi"]', b'topology[server]["ordinary_rps"]')
            data = replace_once(data, b'group, addr, self.napi\n', b'group, addr, [{"id": detail["socket_napi_id"]}]\n')
        elif name == "router/run.py":
            data = replace_once(data, b'from assess import assess\n', b'from assess_rps import assess\n')
        path.write_bytes(data)
    if sha((root / "tap-split/relay.c").read_bytes()) != proof["reproduction_relay_c_sha256"]:
        raise ValueError("checkout relay source differs from lifecycle provenance")
    if sha((root / "router/assess.py").read_bytes()) != proof["reproduction_router_assess_py_sha256"]:
        raise ValueError("checkout common scoring/health assessment differs from provenance")
    if existing:
        binary = existing.resolve()
        mode = "original scored ELF, externally supplied"
    else:
        build = root / "tap-split"
        binary = build / "relay"
        subprocess.run([compiler, "-O3", "-g", "-Wall", "-Wextra", "-Werror", "-std=gnu11", "-pthread",
                        "-ffile-prefix-map=" + str(build) + "=.", "relay.c", "-o", "relay"], cwd=build, check=True)
        mode = "current checkout relay with orphan-safe owner lifecycle"
    files = {name: sha((root / name).read_bytes()) for name in (*COMMON, *ADDED)}
    manifest = dict(files=files, primary_shape_sha256=PRIMARY_SHAPE, relay_sha256=sha(binary.read_bytes()), stock_mload_sha256=MLOAD)
    write_json(root / "manifest.json", manifest)
    provenance = {"reference_git": PIN, "shared_sources_sha256": source_digest,
                  "original_primary_shape_sha256":PRIMARY_SHAPE,
                  "historical_scoring":{k:proof[k] for k in ("scoring_elf_sha256","scoring_relay_c_sha256","scoring_router_assess_py_sha256")},
                  "checkout_reproduction":{k:proof[k] for k in ("reproduction_elf_sha256","reproduction_relay_c_sha256","reproduction_router_assess_py_sha256")},
                  "selected_relay":mode,"resolved_runtime_elf":str(binary),"runtime_elf_sha256":manifest["relay_sha256"],
                  "matches_original_scoring_elf":manifest["relay_sha256"] == proof["scoring_elf_sha256"],
                  "matches_documented_checkout_rebuild":manifest["relay_sha256"] == proof["reproduction_elf_sha256"],
                  "manifest_sha256":sha((root/"manifest.json").read_bytes())}
    if not existing:
        provenance["compiler"] = subprocess.check_output([compiler,"--version"],text=True).splitlines()[0]
    write_json(work / "provenance.json", provenance)
    for name in (*COMMON, *ADDED, "manifest.json"):
        (root / name).chmod(0o444)
    print(json.dumps(provenance,indent=2,sort_keys=True))


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--checkout", required=True, type=Path)
    p.add_argument("--work", required=True, type=Path, help="fresh local work directory")
    p.add_argument("--archival-relay", type=Path, help="optional existing original scoring ELF; fingerprint is verified")
    p.add_argument("--cc", default="cc")
    args = p.parse_args()
    prepare(args.checkout.resolve(strict=True), args.work.resolve(), args.archival_relay, args.cc)
