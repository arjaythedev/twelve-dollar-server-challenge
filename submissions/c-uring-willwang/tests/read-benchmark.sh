#!/usr/bin/env bash
# From the repository root:
# bash submissions/c-uring-willwang/tests/read-benchmark.sh [--rank] [seed.db] [requests] [rounds]
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
leader="$here/../rust-iouring-kensac"
comparison=kensac
if [[ ${1:-} == --rank ]]; then comparison=rank; shift; fi
seed=${1:-"$here/../../seed/feed.db"}
requests=${2:-750000}
rounds=${3:-3}
[[ -f "$seed" ]] || { echo "Missing seed; run bash seed/make-seed.sh first." >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

# Use the submission's actual release objects. Build the scored Rust reader
# and SQLite shim with Kensac's own build.rs, Cargo.lock and release profile.
bash "$here/build.sh"
cc_args=(-fuse-ld=gold -flto -O3 -march=native -fno-plt -std=gnu11 -I"$here" -I"$here/vendor")
libs=(-lcrypto -pthread -lm -ldl)
if [[ $comparison == rank ]]; then
    # Import the shipping CPPFLAGS, CFLAGS and pinned SQLite options directly.
    make -C "$here" -f Makefile -f - CONTROL="$work/control.o" "$work/control.o" <<'MAKE'
$(CONTROL): tests/rank-control.c reader.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SQLITE_FLAGS) -c $< -o $@
MAKE
    "${CC:-clang-18}" "${cc_args[@]}" -DBENCH_RANK "$here/tests/read-benchmark.c" \
        "$here"/build/{database,wal,buffer}.o "$work/control.o" \
        "${libs[@]}" -o "$work/paired"
else
    mkdir -p "$work/rust/src"
    cp "$leader"/{Cargo.toml,Cargo.lock,build.rs} "$work/rust/"
    printf '\n[lib]\ncrate-type = ["staticlib"]\n' >> "$work/rust/Cargo.toml"
    ln -s "$leader/csrc" "$work/rust/csrc"
    ln -s "$here/vendor" "$work/rust/vendor"
    ln -s "$leader/src/db.rs" "$leader/src/json.rs" "$work/rust/src/"
    cp "$here/tests/read-kensac.rs" "$work/rust/src/lib.rs"
    export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
    "${RUSTC:-rustc}" --version
    "${CARGO:-cargo}" --version
    CARGO_TARGET_DIR="$work/target" "${CARGO:-cargo}" build --release --locked --manifest-path "$work/rust/Cargo.toml"

    "${CC:-clang-18}" "${cc_args[@]}" "$here/tests/read-benchmark.c" \
        "$here"/build/{database,wal,reader,buffer}.o "${libs[@]}" -o "$work/candidate"
    "${CC:-clang-18}" "${cc_args[@]}" -DBENCH_KENSAC "$here/tests/read-benchmark.c" \
        "$work/target/release/libfeed_server.a" "${libs[@]}" -o "$work/kensac"
fi

python3 - "$work" "$seed" "$requests" "$rounds" "$comparison" <<'PY'
import json, os, pathlib, shutil, statistics, subprocess, sys, tempfile
work, seed, requests, rounds, comparison = sys.argv[1:]
cpu = int(os.environ.get("READ_BENCH_CPU", min(os.sched_getaffinity(0))))
rounds = int(rounds)
if rounds < 1:
    raise SystemExit("rounds must be positive")
paired = comparison == "rank"
results = {group: [] for group in (("stable", "moving") if paired else ("candidate", "kensac"))}
expected = {}
for n in range(rounds):
    order = list(results) if n % 2 == 0 else list(reversed(results))
    for name in order:
        with tempfile.TemporaryDirectory(dir=work) as sample:
            path = pathlib.Path(sample) / "feed.db"
            shutil.copyfile(seed, path)
            binary = pathlib.Path(work) / ("paired" if paired else name)
            args = [str(binary), str(path), requests] + ([name] if paired else [])
            run = subprocess.check_output(["taskset", "-c", str(cpu), *args])
            result = json.loads(run)
        print(f"round {n + 1} {name:9} {result}", flush=True)
        fingerprint = result["feeds"], result["posts"], result["sha256"]
        key = name if paired else "moving"
        if expected.setdefault(key, fingerprint) != fingerprint:
            raise SystemExit("different read counts or response SHA-256")
        results[name].append(result)
for key, fingerprint in expected.items():
    print(f"{key} identical SHA-256 {fingerprint[2]} on CPU {cpu}")

def show(label, a, first, b, second):
    print(f"{label} median of round means: {a} {first:.1f}, {b} {second:.1f}, "
          f"change {(first / second - 1) * 100:+.1f}%")

if paired:
    for group, samples in results.items():
        show(f"{group} feed_ns", "rank", statistics.median(s["rank_ns"] for s in samples),
             "all 20", statistics.median(s["all_ns"] for s in samples))
else:
    for metric in ("feed_ns", "post_ns"):
        show(metric, "candidate", statistics.median(s[metric] for s in results["candidate"]),
             "Kensac", statistics.median(s[metric] for s in results["kensac"]))
PY
