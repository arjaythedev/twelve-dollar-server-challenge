# Resumed qualification session

Goal explicitly reactivated after the testing block; parallel final validation and research subsequently requested. At measurement time, no commits, pushes, paid provisioning or production changes had been performed. Measured baseline ASM source remains unchanged.

| Local 275k short screen | Requests | HTTP failure rate | p95 ms | p99 ms | Peak server MiB | Gate |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| NASM #75 | 2,213,345 | 0.00000000 | 882.828 | 3583.575 | 625.8 | fail |
| NASM current | 2,343,445 | 0.00027609 | 184.757 | 4562.447 | 1589.8 | fail |
| NASM IO diagnostic replay | 2,411,498 | 0.00000000 | 14.630 | 36.260 | 1391.6 | pass |

All used unchanged bench/load.js, 60s ramp/30s hold/no warmup, private loopback, k6 1.4.2 with92GiB outside server quota, 1CPU/2GiB/no swap/nofile65535. No server or generator OOM. #75 and current candidate were sequential; the diagnostic replay is instrumentation, not a matched uninstrumented win. Short screens weight ramp transients differently from five-minute confirmations. The earlier275k five-minute zero-failure pass remains valid for its exact artifact; neither a maximum nor robust whole-field ranking is established.

Current candidate consumed about31.10us serverCPU/request versus40.35us for#75, including ramp/accept/close work. Better CPU efficiency did not make every latency percentile better.

Diagnostic replay recorded zero ENOBUFS, zero input/output admission failures and peak26,607,616 output bytes. CQ depth reached8192 during end-of-run closure. This does not reproduce retry starvation or explain the failed preceding run.

Generator contention remains a hypothesis. Go GOMAXPROCS controls simultaneous executing goroutines; it does not set server affinity or a kernel limit ([official Go FAQ](https://go.dev/doc/faq#GOMAXPROCS)). Optional generator parallelism and GCtrace controls now record their configuration explicitly. Any new comparisons must use identical generator settings; no advantage will be credited to application changes solely from such controls.

The harness now retains server terminal-state/sample evidence even on crash. Status checks are explicitly distinct from full JSON semantics. Its old zero-check-failure acceptance was stricter than the challenge: transport check misses explained by the allowed HTTP-failure count no longer impose an extra zero-error threshold. Unexplained status-check misses still reject; full semantics are separately validated. Both failed pair results above failed latency regardless, and previous zero-error confirmation is unaffected.

Parallel validation: research/final-validation.md; isolated unapplied timer candidate: research/bounded-timers-experiment.md; opt-in retry proposal: research/progress-retries.patch. No capacity gain claimed for unmeasured proposals.
