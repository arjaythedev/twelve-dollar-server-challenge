# NASM inline literal-copy experiment

The candidate replaces repeated `append_z` scans for eleven fixed response
fragments with compile-time-sized 8/4/2/1-byte NASM copies. Dynamic strings
retain the scalar routine. `--literal-copy=inline` selects the new path;
the default remains scalar because the official-workload result was mixed.

Validation on the scratch candidate binary (`c03564c2…`, SQLite build identity
`9b407e48…`): primitive tests passed, focused integration passed in both modes,
and the unchanged challenge suite passed 42/42 with scalar default and with
inline explicitly selected. The guard-page/canary harness covered all eleven
literals at sixteen destination alignments.

Two paired 12-second same-host saturation screens (Node 24.15.0/autocannon 8.0.0,
one CPU, 2 GiB, same image and seed) favored inline requests per CPU-second by
3.3% and 2.3%. This is substitute-generator evidence only.

The unchanged k6 workload was screened at 275,000 VUs, 60-second ramp,
30-second hold, 30-second ramp-down, 92-GiB generator cap, same image and
server limits. In the first pair, inline reached 18,033 req/s vs scalar 17,627;
inline p95/p99 were 104/1,671 ms vs scalar 824/7,304 ms. Scalar had 509
transport/status failures; inline had zero. Both runs exited 99 on latency
thresholds. In the reverse-order pair, scalar reached 18,492 req/s and inline
18,362; p95/p99 were 18.9/139 ms for scalar and 19.3/169 ms for inline. Both
had zero request/check failures and exited 0.

The first-pair tail outlier did not repeat. These short, variable results do
not establish a capacity increase or repeatable official-workload gain, so the
inline path remains opt-in. A five-minute hold and comparable contender
qualification were not run for this candidate.
