# SQLite and Go profiles (enabled by default)

`../build.sh` enables Go PGO, SQLite PGO and ThinLTO by default, using
`go.pprof` and `sqlite.profraw`. Use `--no-go-pgo` to disable Go PGO, `--no-pgo`
to disable SQLite PGO or `--no-thinlto` to disable ThinLTO; the options are
independent. `--baseline` disables both PGO modes and selects v1, keeping
ThinLTO unless explicitly disabled; custom Go profiles conflict with baseline.
The build merges `sqlite.profraw` with LLVM 23.1.3 `llvm-profdata` and applies it
to the pinned SQLite C amalgamation.
Go and SQLite PGO use separate profiles and validation; disabling either leaves
the other enabled.

- `sqlite.profraw`: LLVM IR execution counters (406,512 bytes).
- `manifest.json`: build compatibility and integrity checks.
- `provenance.json`: source hashes, training compiler flags and export receipt.

Raw SHA-256:
`8446142f0e5d1a9692f88c1ee5c6a9270532932271a8c772e45ae924359ed974`.

Training used Clang 23.1.3, O2, x86-64-v3, SQLite 3.53.4 from mattn v1.14.52,
`-fprofile-generate -fprofile-update=atomic`, and Go 1.27.2 with Go PGO off.
Only SQLite was instrumented. The official k6 workload ran at 40k VUs after
warmup. Counters were written only after graceful shutdown joined all SQLite
owners; the receipt binds the raw hash to that training executable and campaign.

The build uses LLVM’s [symbol remapping](https://llvm.org/docs/CommandGuide/llvm-profdata.html)
to relocate static-function names from the recorded training source path to the
current checkout. It verifies that every function hash and execution counter
remains unchanged. No original training directory is needed, and strict
missing/stale-profile diagnostics remain enabled.

The file is aggregate execution metadata, not the seeded database, user content,
request logs, cached responses or secrets. Normal build and startup do not run a
training load. Baseline and PGO-use executables do not link the training runtime.

To reproduce a new profile, build the same pinned SQLite source and definitions
with LLVM IR instrumentation and atomic counters, link compiler-rt and the
included `sqlite_llvm_pgo`-tagged export hook, run a representative mixed workload
against a fresh challenge seed, then stop gracefully. The hook requires a fresh
absolute `GOAL_LLVM_PROFILE` path and embedded training identity; it exports only
after the SQLite workers join. Review source/flag changes and record a new
manifest and provenance before replacing this profile. The ordinary build CLI
intentionally exposes only baseline and profile-use modes, not a training runner.

The historical training was performed separately from the contest. Shipping this
default-use file is disclosed for organizer review, not presented as prior approval.

## Go CPU profile (default)

- `go.pprof`: gzip pprof CPU samples, **264.69 seconds** sampled CPU.
- `go-provenance.json`: original training source hashes, binary identity,
  Go/CPU/platform settings and workload/shutdown disclosure.
- SHA-256: `cffbedaaa8f7208e6cce79844a858e8ae2f08a3a14079cdc345f0968e63eb6ff`.

Training used Go 1.27.2 on Linux/amd64 v3, with SQLite PGO + ThinLTO enabled
and Go PGO disabled. A separate Rust diagnostic workload used 100,000 VUs after
warmup. After load the SSH control connection was lost; graceful shutdown under
the outer 1200-second watchdog finalized the profile. Samples include post-load
idle time. The collection run is not accepted as a benchmark result, and this
profile is not evidence of maximum supported load. Original training provenance
is retained even though later JWT source and packaging differ.

Ordinary builds resolve this bundled profile relative to the builder and verify
its pinned SHA-256 above. `make go-pgo` / `make test-go-pgo` remain explicit
equivalents of those optimization settings. Use `--no-go-pgo` to opt out, or
replace it with the paired `--go-pgo PATH --go-pgo-sha256 HASH` interface in the
main README. Both custom options are required together and conflict with
`--no-go-pgo` and `--baseline`. No training runs during build or startup.
Application build/tests use the selected profile, while the CPU probe always
uses `-pgo=off`. File/hash,
pprof structure, CPU units, positive CPU and compiler-selected signed weights are
validated; the build receipt binds profile identity and rechecks it before output.
Missing or invalid bundled/custom profiles fail, including with cached SQLite
objects; there is no silent fallback or implicit `default.pgo` selection.

The recorded three-run 120k diagnostic used this exact Go profile with SQLite
PGO + ThinLTO, matching the new default optimization settings. The CLI default
change also passed a native `./build.sh --test`, all 42 API checks and the
`MAX_CONNECTIONS=1` regression on the server VM. No benchmark rerun was needed. The measured binary SHA-256 is
`5761f1c4dae4ae22c4261a78308d69f0a79364952368e70e779be82b3224d90a`.
See the main README for results and measurement limits.

This CPU profile records sampled stacks and timing metadata, not a database or
cached responses. Its training workload was diagnostic and separate from the
contest. Profile provenance/use is disclosed for organizer review.
