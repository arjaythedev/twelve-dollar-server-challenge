These are real gzip pprof CPU profiles derived from the Go 1.27.2 toolchain's
`src/cmd/compile/internal/test/testdata/pgo/inline/inline_hot.pprof`, under the
included Go BSD license. Recorded stacks, functions (including start lines),
mappings and timing metadata are retained. Only sample columns/values/units change:

- `positive.pprof`: CPU-only, `cpu/nanoseconds`, total 1,300,000,000 ns.
- `zero.pprof`: the same recorded stacks and units, all CPU values zero.
- `wrong-unit.pprof`: positive `cpu/bytes`, with no `samples/count` column.
- `count-first-zero.pprof`: `samples/count=0`, then CPU total 1,300,000,000 ns.
- `count-first-positive.pprof`: `samples/count=130`, then CPU total 1,300,000,000 ns.
- `cpu-first-zero.pprof`: CPU total zero, then `samples/count=130`.
- `cpu-first-positive.pprof`: CPU total 1,300,000,000 ns, then `samples/count=0`.
- `count-first-zero-cpu.pprof`: `samples/count=130`, then CPU total zero;
  preserves the independent positive CPU weight requirement.

All eight parse successfully with
`go tool pprof -top -nodecount=1 -sample_index=cpu -symbolize=none FILE`.
Exit status alone cannot distinguish them.

Go 1.27.2 `src/cmd/internal/pgo/pprof.go:FromPProf` walks sample types in order
and selects the **first** exact `samples/count` or `cpu/nanoseconds` pair.
`src/cmd/preprofile/main.go:preprocess` calls this same compiler PGO parser.
Tests run the installed `go tool preprofile -i FILE` and inspect serialized
edges, rather than relying on pprof display totals or a mocked compiler:

- Count-first zero and CPU-first zero produce just `GO PREPROFILE V1`, no edges.
- Count-first positive produces four edges with total weight 135.
- CPU-first positive produces four edges with total weight 1,350,000,000,
  despite its zero count column.
- Count-first zero-CPU produces four edges with total weight 135, but the
  validator still rejects it because CPU weight must independently be positive.

Edge totals count call edges, so they differ from sample totals. The validator
checks signed sample totals for both CPU and the compiler-selected column.
It retains the first-CPU exact-unit guard, digest/path checks, fail-before-build
behavior, stale-output removal and profile mutation checks.

Source fixture SHA-256:
`f021feb8375fc3ace6eddf03a91a80cfdd8a85bdb2db7d4ae4149e91944012ff`.

These small fixtures and the Go BSD license support the real pprof/preprofile
checks without compiler source or training/benchmark artifacts. Separately, the
build pipeline tests copy the bundled `profiles/go.pprof` into their temporary
submission tree to exercise default Go PGO with the pinned digest. Control
cases that require Go PGO off pass `--no-go-pgo` explicitly; baseline disables
both PGO modes. No fixture profile replaces the bundled default implicitly.
