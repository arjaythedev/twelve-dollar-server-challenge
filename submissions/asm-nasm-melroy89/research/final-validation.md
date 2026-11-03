# Final validation audit — 2026-10-14

This audit adds lightweight correctness evidence without rebuilding shared outputs,
changing measured assembly, or running another capacity test. Root runs comparative
benchmarks separately. No commit, push, provisioning or PR action was performed.

## Artifact correspondence

All nine current `src/` files SHA-256 match both
`results/optimized-package-evidence.json` and
`results/optimized-275000-confirmation-evidence.json`. Both package backends and
the population run identify the same measured release binary
`e511302626f6f0e06c2981aeb19e4d90923afeebf497ff711373d791f9a8491e`
and image
`sha256:4438adff52e1ed4db5e941ae1f3ab5ee0b64d4cfd6ae850694969e4d7186fb16`.
Current unchanged `bench/load.js` SHA matches the population manifest.

The native `bin/server` is a different build artifact,
`e644581927cb42389273b5fe04602833068c16e5d3e2055a65a9e0dce2707d13`,
correctly recorded in native `build/identity.json`; its recorded source hashes
match the measured image. Native correctness does not transfer a measured
capacity score between binaries.

Recorded package evidence supports normal-user foreground startup and 42/42
checks on io_uring and epoll, healthy at 0.247/0.203 seconds. Dockerfile confirms
root install followed by normal-user source build. These are existing recorded
checks, not a new clean installation performed by this audit.

Recorded full confirmation supports 275,000 selected users, five-minute hold,
standard warmup/ramp, 10,264,833 requests, zero HTTP/status-check failures,
p95 16.233 ms, p99 65.394 ms, peak server memory 1,516,208,128 bytes and no OOM.
Maximum observed request duration was 2,436.599 ms; the official thresholds are
percentiles, so this maximum does not itself contradict a pass.

## Additional native validation

A tiny disposable database copied only canonical CREATE TABLE/INDEX definitions
from the seed and contained two users and one initial post. No full seed copy,
shared rebuild, performance workload or persistent service was used. Port 3199
was released after the test.

Fourteen cases passed against native default settings:

- HS384 and none algorithm rejection despite an otherwise valid HMAC.
- Wrong signature, numeric/null sub, numeric username, string/missing/expired exp.
- Invalid authentication precedes malformed-ID errors; valid authentication then
  receives the malformed-ID 400.
- Missing-user write receives 500, proving foreign-key enforcement; the following
  valid Unicode write receives 201 and can be read back.
- Duplicate likes return 201 then 200.
- WAL persists after shutdown and integrity_check returns ok.

Raw evidence: `results/final-validation-native.json` and server log.
The test took well below one second and did not benchmark throughput.

## Important interpretation and remaining gates

`bench/load.js` check() predicates inspect HTTP statuses. Its zero failed checks
are not an exhaustive JSON, ordering or persistence oracle. Use “zero HTTP and
status-check failures” for this benchmark. Differential/integration/primitive
checks supply separate semantic evidence. Existing PERFORMANCE.md wording
“semantic check failures” should be clarified before public submission.

Current source explicitly sets WAL/NORMAL/FK ON and prepares readback queries;
120/30-second timeout constants are present. This short audit does not independently
wait for the 120-second idle or 30-second stalled-send retirement limits.

The script-provenance gap is now closed by read-only inspection of the actual
measured image. Docker image ID matches the recorded sha256:4438adff... identity.
sha256sum executed directly with networking disabled, without starting any
server, installer or build. Host install.sh, build.sh, start.sh and tools/build.py
all match their image counterparts (/tmp/install.sh and /app paths). All nine
assembly/include files also match, and image bin/server matches recorded e511.
Native bin/server remains the separately identified e644 build. Exact hashes:
results/final-script-provenance.json. Dockerfile and source-only staged inventory
still require review before publication; they are not established by script
hash equality alone.

The updated benchmark harness now separates status checks from JSON semantics
and preserves the official less-than 1% HTTP failure threshold through k6's exit
status. Status-check misses up to the counted HTTP failures can be explained by
transport failures; unexplained additional misses fail the gate. JSON semantics
are explicitly separate. The earlier zero-missed-check gate was stricter than the
rules. This correction does not alter the historical 275k zero-error pass.

Whole-field eligibility, comparable maximum capacity, actual Ubuntu target-kernel
proof and separate physical generator remain unproven. The tested 275k population
is not a maximum search. A comparable contender pass must not be described as a
NASM victory. No new production defect was found in these selected tests.

For PR preparation, omit TMP-PR-DESC.md/local handoffs if intended only for local
use and exclude ignored bin/build/deps/results trees. Preserve the raw evidence
locally. PR submission readiness and a proven competition win are distinct.
