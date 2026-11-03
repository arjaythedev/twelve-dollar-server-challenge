# Benchmarks

2026-11-01 to 2026-11-03. Ryzen 7 7800X3D, Ubuntu 24.04 in Docker Desktop/WSL2. Server: 1 CPU, 2 GiB, no container swap. k6 used the official workload and a fresh database per run, on Windows unless labeled Linux loopback (32 source IPs).

| Runtime | Users | Hold | Requests | Requests/s | p95 | p99 | Max | Errors | CPU/request | Peak RAM |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Python 3.12.13 | 1,000 | 60 s | 10,346 | 79.58 | 1.172 ms | 1.594 ms | 28.03 ms | 0% | 426.84 us | 308.68 MiB |
| Python 3.12.13 | 5,000 | 60 s | 51,795 | 398.42 | 1.125 ms | 3.069 ms | 584.91 ms | 0% | 287.85 us | 327.07 MiB |
| Python 3.12.13 | 5,000 | 5 min | 186,850 | 467.12 | 1.044 ms | 1.459 ms | 35.02 ms | 0% | 273.54 us | 323.30 MiB |
| Python 3.15.2 | 5,000 | 5 min | 186,708 | 466.77 | 1.044 ms | 1.564 ms | 61.09 ms | 0% | 269.54 us | 388.42 MiB |
| Python 3.15.2 + C JSON | 5,000 | 60 s | 51,870 | 399.05 | 1.086 ms | 1.610 ms | 121.65 ms | 0% | 264.36 us | 336.33 MiB |
| C++ 0.1.0 | 5,000 | 60 s | 51,865 | 398.95 | 1.048 ms | 1.568 ms | 189.80 ms | 0% | 132.13 us | 304.56 MiB |
| C++ 0.1.0 (repeat) | 5,000 | 60 s | 51,909 | 399.31 | 1.050 ms | 1.552 ms | 184.86 ms | 0% | 131.93 us | 302.72 MiB |
| C++ 0.1.0 (uring-base, Windows) | 10,000 | 60 s | 103,259 | 794.27 | 1.060 ms | 2.074 ms | 45.99 ms | 0% | 84.23 us | 348.19 MiB |
| C++ 0.1.0 (uring-coop, Windows) | 10,000 | 60 s | 103,820 | 798.59 | 1.050 ms | 1.605 ms | 33.10 ms | 0% | 82.84 us | 343.96 MiB |
| C++ 0.1.0 (uring-batch, Windows) | 10,000 | 60 s | 103,745 | 798.01 | 2.609 ms | 5.363 ms | 34.67 ms | 0% | 81.92 us | 340.11 MiB |
| C++ 0.1.0 (uring-base, Linux loopback) | 50,000 | 60 s | 518,222 | 3,984.46 | 0.584 ms | 16.421 ms | 55.84 ms | 0% | 54.58 us | 681.55 MiB |
| C++ 0.1.0 (uring-batch, Linux loopback) | 50,000 | 60 s | 457,791 | 3,519.70 | 4,804.318 ms | 5,586.012 ms | 54,987.35 ms | 1.94% | 47.02 us | 672.58 MiB |
| C++ 0.1.0 (adaptive, Linux loopback) | 50,000 | 60 s | 518,877 | 3,989.27 | 1.478 ms | 18.388 ms | 55.68 ms | 0% | 46.25 us | 692.12 MiB |
| C++ 0.1.0 (B-tree feed, Linux loopback) | 50,000 | 60 s | 518,213 | 3,984.43 | 1.474 ms | 20.657 ms | 108.56 ms | 0% | 45.00 us | 687.91 MiB |
| C++ 0.1.0 (2-ms wait, Linux loopback) | 50,000 | 60 s | 518,114 | 3,983.08 | 2.753 ms | 21.918 ms | 263.26 ms | 0% | 44.06 us | 684.10 MiB |
| C++ 0.1.0 (deferred taskwork, Linux loopback) | 50,000 | 60 s | 518,430 | 3,986.05 | 1.632 ms | 21.604 ms | 132.07 ms | 0% | 46.12 us | 699.32 MiB |
| C++ 0.1.0 (B-tree feed repeat, Linux loopback) | 50,000 | 60 s | 518,190 | 3,984.10 | 1.536 ms | 21.952 ms | 118.30 ms | 0% | 45.70 us | 682.38 MiB |
| C++ 0.1.0 (range/local + 2-ms wait, Linux loopback) | 50,000 | 60 s | 518,437 | 3,986.13 | 2.409 ms | 18.236 ms | 69.87 ms | 0% | 38.86 us | 692.95 MiB |
| C++ 0.1.0 (range/local + 2-ms repeat, Linux loopback) | 50,000 | 60 s | 517,392 | 3,978.10 | 2.587 ms | 21.344 ms | 104.77 ms | 0% | 40.02 us | 686.48 MiB |
| C++ 0.1.0 (shared receive pool, Linux loopback) | 50,000 | 60 s | 517,485 | 3,978.64 | 2.448 ms | 19.221 ms | 106.41 ms | 0% | 38.78 us | 466.52 MiB |
| C++ 0.1.0 (scatter-gather sends, Linux loopback) | 50,000 | 60 s | 518,431 | 3,986.25 | 2.403 ms | 20.840 ms | 114.77 ms | 0% | 37.27 us | 422.15 MiB |
| C++ 0.1.0 (32 CQEs / 4-ms wait, Linux loopback) | 50,000 | 60 s | 518,522 | 3,986.87 | 4.646 ms | 22.405 ms | 112.91 ms | 0% | 33.52 us | 422.06 MiB |
| C++ 0.1.0 (64 CQEs / 8-ms wait, Linux loopback) | 50,000 | 60 s | 517,863 | 3,981.45 | 8.645 ms | 22.215 ms | 97.42 ms | 0% | 33.64 us | 422.19 MiB |
| C++ 0.1.0 (direct-parse baseline, Linux loopback) | 50,000 | 60 s | 517,785 | 3,980.96 | 4.652 ms | 21.708 ms | 106.82 ms | 0% | 34.27 us | 421.78 MiB |
| C++ 0.1.0 (direct receive parsing, Linux loopback) | 50,000 | 60 s | 518,140 | 3,983.96 | 4.661 ms | 21.401 ms | 111.44 ms | 0% | 34.73 us | 405.88 MiB |
| C++ 0.1.0 (low-hanging baseline A, Linux loopback) | 25,000 | 60 s | 259,411 | 1,995.17 | 4.643 ms | 15.593 ms | 99.42 ms | 0% | 39.79 us | 341.71 MiB |
| C++ 0.1.0 (low-hanging A, Linux loopback) | 25,000 | 60 s | 259,483 | 1,995.82 | 4.632 ms | 15.321 ms | 91.17 ms | 0% | 39.34 us | 341.66 MiB |
| C++ 0.1.0 (low-hanging baseline B, Linux loopback) | 25,000 | 60 s | 259,468 | 1,995.52 | 4.645 ms | 14.754 ms | 99.35 ms | 0% | 39.69 us | 341.40 MiB |
| C++ 0.1.0 (low-hanging B, Linux loopback) | 25,000 | 60 s | 259,229 | 1,993.62 | 4.640 ms | 15.178 ms | 95.01 ms | 0% | 39.48 us | 341.59 MiB |
| C++ 0.1.0 (direct-descriptor baseline A, Linux loopback) | 25,000 | 60 s | 259,288 | 1,993.44 | 4.634 ms | 14.389 ms | 68.82 ms | 0% | 40.46 us | 343.40 MiB |
| C++ 0.1.0 (direct descriptors A, Linux loopback) | 25,000 | 60 s | 259,013 | 1,991.54 | 4.640 ms | 14.730 ms | 65.56 ms | 0% | 40.49 us | 342.45 MiB |
| C++ 0.1.0 (direct-descriptor baseline B, Linux loopback) | 25,000 | 60 s | 259,159 | 1,992.99 | 4.634 ms | 14.107 ms | 92.20 ms | 0% | 40.56 us | 341.35 MiB |
| C++ 0.1.0 (direct descriptors B, Linux loopback) | 25,000 | 60 s | 259,214 | 1,993.26 | 4.639 ms | 14.077 ms | 92.99 ms | 0% | 40.49 us | 342.55 MiB |
| C++ 0.1.0 (refactor baseline, Linux loopback) | 50,000 | 60 s | 519,120 | 3,990.66 | 4.681 ms | 21.253 ms | 125.79 ms | 0% | 34.47 us | 431.94 MiB |
| C++ 0.1.0 (compact transport, Linux loopback) | 50,000 | 60 s | 518,181 | 3,983.89 | 4.612 ms | 20.346 ms | 117.51 ms | 0% | 33.59 us | 407.12 MiB |
| C++ 0.1.0 (native client, Linux loopback) | 100,000 | 60 s | 1,036,376 | 7,972.07 | 11.246 ms | 26.269 ms | 120.47 ms | 0% | 32.54 us | 653.45 MiB |
| C++ 0.1.0 (hybrid send, Linux loopback) | 100,000 | 60 s | 1,036,840 | 7,975.70 | 12.957 ms | 33.165 ms | 125.39 ms | 0% | 32.52 us | 647.02 MiB |
| C++ 0.1.0 (cold Exchange, Linux loopback) | 100,000 | 60 s | 1,036,362 | 7,972.03 | 11.366 ms | 27.424 ms | 455.02 ms | 0% | 31.05 us | 528.11 MiB |
| C++ 0.1.0 (native client, failed) | 300,000 | 5 min | 10,284,638 | 25,711.54 | 5,316.581 ms | 19,524.643 ms | 60,001.25 ms | 0.0066% | 25.80 us | 1,391.69 MiB |
| C++ 0.1.0 (bounded handler queue) | 100,000 | 60 s | 1,037,202 | 7,978.44 | 12.390 ms | 32.713 ms | 201.77 ms | 0% | 31.32 us | 525.76 MiB |
| C++ 0.1.0 (checkpoint worker) | 100,000 | 60 s | 1,037,228 | 7,978.67 | 3.690 ms | 4.711 ms | 34.51 ms | 0% | 32.70 us | 535.91 MiB |
| C++ 0.1.0 (checkpoint worker, failed) | 300,000 | 5 min | 11,039,291 | 27,598.20 | 75.035 ms | 3,979.977 ms | 33,316.56 ms | 0% | 27.86 us | 1,857.30 MiB |
| C++ 0.1.0 (300k profile) | 300,000 | 60 s | 3,586,049 | 22,412.72 | 5.080 ms | 8.026 ms | 94.87 ms | 0% | 28.42 us | 1,522.39 MiB |
| C++ 0.1.0 (idle poll, reverted) | 300,000 | 60 s | 3,542,624 | 22,141.34 | 72.329 ms | 4,170.837 ms | 7,246.31 ms | 0% | 30.26 us | 1,993.39 MiB |
| C++ 0.1.0 (feed batch) | 300,000 | 60 s | 3,586,146 | 22,413.30 | 5.067 ms | 8.487 ms | 139.13 ms | 0% | 28.32 us | 1,361.25 MiB |
| C++ 0.1.0 (feed batch) | 300,000 | 5 min | 11,148,015 | 27,869.99 | 16.217 ms | 749.363 ms | 23,529.85 ms | 0% | 26.69 us | 1,618.10 MiB |
| C++ 0.1.0 (diagnostics) | 300,000 | 5 min | 10,846,881 | 27,117.20 | 390.067 ms | 9,904.171 ms | 38,276.73 ms | 0% | 26.47 us | 2,354.75 MiB |
| C++ 0.1.0 (feed batch control, failed) | 300,000 | 5 min | 11,139,199 | 27,847.99 | 134.529 ms | 1,702.041 ms | 14,729.54 ms | 0% | 26.45 us | 1,982.40 MiB |
| C++ 0.1.0 (group commit) | 300,000 | 60 s | 3,581,785 | 22,386.04 | 28.255 ms | 371.544 ms | 4,785.60 ms | 0% | 25.95 us | 1,097.05 MiB |
| C++ 0.1.0 (group commit) | 300,000 | 5 min | 11,200,015 | 28,000.03 | 5.360 ms | 82.080 ms | 3,954.01 ms | 0% | 24.36 us | 1,247.85 MiB |
| C++ 0.1.0 (batch cleanup) | 300,000 | 5 min | 11,207,115 | 28,017.76 | 4.828 ms | 6.775 ms | 159.68 ms | 0% | 23.62 us | 1,294.88 MiB |
| C++ 0.1.0 (batch cleanup, failed) | 500,000 | 60 s | 1,821,037 | 11,381.47 | 58,471.894 ms | 84,766.683 ms | 87,071.71 ms | 6.60% | 32.58 us | 1,277.16 MiB |
| C++ 0.1.0 (typed pipeline) | 300,000 | 60 s | 3,586,201 | 22,413.76 | 4.675 ms | 6.174 ms | 37.62 ms | 0% | 25.16 us | 1,026.10 MiB |
| C++ 0.1.0 (typed pipeline) | 300,000 | 5 min | 11,206,740 | 28,016.80 | 4.818 ms | 6.439 ms | 86.89 ms | 0% | 23.81 us | 1,107.38 MiB |
| C++ 0.1.0 (rewrite phase 1, 64 IPs) | 300,000 | 60 s | 3,586,160 | 22,413.42 | 4.829 ms | 6.554 ms | 30.20 ms | 0% | 26.11 us | 1,005.14 MiB |
| C++ 0.1.0 (rewrite phase 2, 64 IPs) | 300,000 | 60 s | 3,585,569 | 22,409.78 | 5.104 ms | 8.526 ms | 218.07 ms | 0% | 26.66 us | 1,051.48 MiB |
| C++ 0.1.0 (rewrite phase 3, 64 IPs) | 300,000 | 60 s | 3,585,622 | 22,410.09 | 5.027 ms | 7.968 ms | 157.08 ms | 0% | 26.51 us | 1,060.18 MiB |
| C++ 0.1.0 (rewrite phase 4, 64 IPs) | 300,000 | 60 s | 3,585,799 | 22,411.15 | 5.433 ms | 8.865 ms | 47.29 ms | 0% | 28.22 us | 1,081.30 MiB |
| C++ 0.1.0 (reference 671fd4e, 64 IPs) | 300,000 | 60 s | 3,586,039 | 22,412.72 | 5.063 ms | 8.395 ms | 63.71 ms | 0% | 26.80 us | 1,005.53 MiB |
| C++ 0.1.0 (rewrite + TCP close, 64 IPs) | 300,000 | 60 s | 3,587,278 | 22,420.39 | 5.025 ms | 7.690 ms | 99.27 ms | 0% | 26.85 us | 914.77 MiB |
| C++ 0.1.0 (before admission, 32 IPs) | 10,000 | 15 s | 42,649 | 710.81 | 4.207 ms | 4.412 ms | 9.57 ms | 0% | 55.22 us | 297.06 MiB |
| C++ 0.1.0 (admission, 32 IPs) | 10,000 | 15 s | 42,328 | 705.46 | 4.203 ms | 4.389 ms | 8.88 ms | 0% | 54.48 us | 292.85 MiB |
| C++ 0.1.0 (rewrite + admission, 32 IPs) | 300,000 | 5 min | 6,294,188 | 15,735.45 | 48,336.231 ms | 60,000.646 ms | 60,011.26 ms | 6.41% | 56.57 us | 2,052.34 MiB |
| C++ 0.1.0 (rewrite, admission removed, 32 IPs) | 300,000 | 5 min | 11,129,340 | 27,823.33 | 43.466 ms | 2,070.333 ms | 8,180.12 ms | 0% | 25.90 us | 1,002.36 MiB |
| C++ 0.1.0 (rewrite diagnostics, 32 IPs) | 300,000 | 90 s | 4,537,988 | 23,884.15 | 4.841 ms | 6.507 ms | 67.49 ms | 0% | 25.98 us | 982.07 MiB |
| C++ 0.1.0 (rewrite diagnostics, 32 IPs) | 300,000 | 5 min | 11,112,126 | 27,780.31 | 9.961 ms | 2,546.114 ms | 25,461.72 ms | 0% | 25.96 us | 1,088.68 MiB |
| C++ 0.1.0 (receive-retry baseline) | 50,000 | 15 s | 212,302 | 3,538.33 | 4.213 ms | 4.512 ms | 11.98 ms | 0% | 37.45 us | 419.60 MiB |
| C++ 0.1.0 (bounded receive retries) | 50,000 | 15 s | 212,665 | 3,544.44 | 4.204 ms | 4.511 ms | 12.44 ms | 0% | 37.12 us | 419.51 MiB |
| C++ 0.1.0 (submission baseline) | 70,000 | 15 s | 297,811 | 4,963.51 | 3.914 ms | 4.460 ms | 11.84 ms | 0% | 35.72 us | 515.79 MiB |
| C++ 0.1.0 (early submission, reverted) | 70,000 | 15 s | 297,318 | 4,955.30 | 3.959 ms | 4.501 ms | 66.55 ms | 0% | 36.50 us | 469.75 MiB |
| C++ 0.1.0 (prepared writes + fixed outcomes) | 70,000 | 15 s | 297,766 | 4,962.74 | 3.838 ms | 4.449 ms | 10.93 ms | 0% | 35.14 us | 501.56 MiB |
| C++ 0.1.0 (bounded retries + prepared writes) | 300,000 | 5 min | 11,206,244 | 28,015.56 | 5.041 ms | 7.163 ms | 130.40 ms | 0% | 24.94 us | 1,043.15 MiB |
| C++ 0.1.0 (bounded generator timers) | 300,000 | 5 min | 11,205,720 | 28,014.27 | 4.875 ms | 6.372 ms | 60.26 ms | 0% | 24.68 us | 961.11 MiB |
| C++ 0.1.0 (CQ 8,192) | 300,000 | 5 min | 11,205,005 | 28,012.47 | 4.925 ms | 6.582 ms | 91.63 ms | 0% | 24.64 us | 948.57 MiB |
| C++ 0.1.0 (primary defer baseline) | 70,000 | 15 s | 297,241 | 4,954.01 | 3.730 ms | 4.445 ms | 9.23 ms | 0% | 35.43 us | 513.67 MiB |
| C++ 0.1.0 (primary defer) | 70,000 | 15 s | 297,618 | 4,960.22 | 3.623 ms | 4.420 ms | 10.88 ms | 0% | 34.24 us | 546.76 MiB |
| C++ 0.1.0 (primary defer + fixed CQEs) | 300,000 | 5 min | 11,205,141 | 28,012.82 | 4.844 ms | 6.391 ms | 96.95 ms | 0% | 23.91 us | 1,038.57 MiB |
| C++ 0.1.0 (ac0dd15 reference) | 350,000 | 5 min | 13,053,479 | 32,633.62 | 5.938 ms | 216.485 ms | 7,944.61 ms | 0% | 22.87 us | 1,213.42 MiB |
| C++ 0.1.0 (recovery + response reuse) | 350,000 | 5 min | 13,074,570 | 32,686.36 | 5.120 ms | 6.818 ms | 77.46 ms | 0% | 22.72 us | 1,290.85 MiB |
| C++ 0.1.0 (recovery + response reuse) | 400,000 | 5 min | 14,941,600 | 37,353.95 | 5.307 ms | 7.510 ms | 77.30 ms | 0% | 22.40 us | 1,611.87 MiB |
| C++ 0.1.0 (unix-excl baseline) | 70,000 | 15 s | 297,528 | 4,958.73 | 3.671 ms | 4.383 ms | 19.82 ms | 0% | 31.76 us | 519.79 MiB |
| C++ 0.1.0 (unix-excl) | 70,000 | 15 s | 297,359 | 4,955.91 | 3.570 ms | 4.353 ms | 9.13 ms | 0% | 30.98 us | 505.56 MiB |
| C++ 0.1.0 (unix-excl + direct like lookup) | 350,000 | 5 min | 13,073,904 | 32,684.68 | 4.481 ms | 5.667 ms | 83.15 ms | 0% | 20.45 us | 1,497.97 MiB |
| C++ 0.1.0 (create readback) | 350,000 | 5 min | 13,070,456 | 32,676.06 | 4.518 ms | 5.721 ms | 86.62 ms | 0% | 20.73 us | 1,862.91 MiB |
| C++ 0.1.0 (WAL reuse, target missed) | 350,000 | 60 s | 682,326 | 3,898.98 | 169.765 ms | 509.506 ms | 1,513.86 ms | 0% | 31.75 us | 856.20 MiB |
| C++ 0.1.0 (WAL limit 0 MiB) | 100,000 | 60 s | 581,721 | 5,540.15 | 79.660 ms | 252.951 ms | 534.84 ms | 0% | 28.65 us | 545.18 MiB |
| C++ 0.1.0 (WAL limit 4 MiB) | 100,000 | 60 s | 582,631 | 5,548.84 | 267.779 ms | 394.208 ms | 801.64 ms | 0% | 29.88 us | 531.62 MiB |
| C++ 0.1.0 (WAL limit 16 MiB) | 100,000 | 60 s | 587,167 | 5,592.04 | 138.052 ms | 339.001 ms | 817.57 ms | 0% | 29.28 us | 542.28 MiB |
| C++ 0.1.0 (no WAL eviction, A/B) | 100,000 | 60 s | 605,395 | 5,765.62 | 64.646 ms | 272.453 ms | 629.07 ms | 0% | 26.84 us | 559.05 MiB |
| C++ 0.1.0 (WAL eviction + limit 0, A/B) | 100,000 | 60 s | 604,047 | 5,752.81 | 47.184 ms | 285.159 ms | 612.76 ms | 0% | 27.65 us | 502.90 MiB |
| C++ 0.1.0 (447fb8a, target missed) | 350,000 | 60 s | 617,412 | 3,858.82 | 397.871 ms | 591.293 ms | 1,311.29 ms | 0% | 30.95 us | 833.96 MiB |
| C++ 0.1.0 (WSL NAT restored, activation gate) | 10,000 | 15 s | 22,216 | 1,110.78 | 4.185 ms | 4.361 ms | 18.41 ms | 0% | 49.50 us | 302.92 MiB |
| C++ 0.1.0 (no WAL eviction, limit 0, NAT) | 350,000 | 60 s | 4,182,233 | 26,138.87 | 4.679 ms | 6.569 ms | 159.70 ms | 0% | 23.61 us | 1,512.96 MiB |
| C++ 0.1.0 (WAL eviction, limit 0, NAT) | 350,000 | 60 s | 4,182,593 | 26,141.17 | 4.517 ms | 5.700 ms | 18.08 ms | 0% | 23.44 us | 956.17 MiB |
| C++ 0.1.0 (leaf-local likes) | 350,000 | 5 min | 13,057,941 | 32,644.78 | 45.295 ms | 225.780 ms | 2,826.03 ms | 0% | 22.74 us | 1,053.83 MiB |
| C++ 0.1.0 (leaf-local likes, sanity) | 350,000 | 5 min | 13,071,770 | 32,679.41 | 5.196 ms | 44.310 ms | 1,868.28 ms | 0% | 22.43 us | 1,070.78 MiB |
| C++ 0.1.0 (per-entry likes, A) | 350,000 | 5 min | 13,074,468 | 32,686.09 | 4.687 ms | 5.986 ms | 58.29 ms | 0% | 22.30 us | 1,031.52 MiB |
| C++ 0.1.0 (leaf-local likes, B) | 350,000 | 5 min | 13,072,068 | 32,680.13 | 4.604 ms | 5.838 ms | 54.75 ms | 0% | 21.87 us | 1,018.46 MiB |

Leaf-local likes, matched 350k/5m A/B, seed 3132383413, no concurrent builds/probes: CPU/request 22.30 to 21.87 us (-1.9%), hold p99 6.02 to 5.86 ms. Both reached 350k on time with complete response coverage and no errors or memory-limit events. Earlier second-long outliers were not reproduced. [Data](src/benchmarks/leaf-likes.json).

Sanity repeat, same binaries, seed 3132383413 and limits: hold p99 59.14 ms, no errors or memory-limit events. Builds and short native probes ran alongside it; tail variability remains unexplained. [Data](src/benchmarks/leaf-likes.json).

WAL-reuse Docker run: 258,352 live connections in hold, 280,899 overall; the 350k target was missed. Server CPU 21.66 s, PSI I/O full 81.93 s (includes network CQ waits); no memory-limit events or OOM kills.

WAL limits 0/4/16 MiB, serial 100k runs, seed 42, 30-second ramp and 60-second hold: hold p99 257.24/405.78/351.85 ms; physical reads 1.87/272.55/380.62 MiB. WSL mirrored networking delayed all ramps; these are startup diagnostics. No errors or memory-limit events. [Data](src/benchmarks/wal-cache.json).

WAL paths, same 100k setup: no eviction/default limit versus eviction/limit 0 gave hold p99 275.06/286.29 ms, peak RAM 559.05/502.90 MiB and PSI I/O full 53.14/52.36 s. Mirrored source-bind overhead delayed both ramps. PSI includes idle network CQ waits. [Data](src/benchmarks/wal-cache.json).

447fb8a, 350k target, 60-second ramp/hold: server and generator match the earlier like-values binaries. Only 244,074 connect attempts; kernel sample at 100.82 s showed 176,563 established pairs. Hold p99 4.94 ms, ramp-down p99 717.01 ms; PSI I/O full 81.83 s. Mirrored source-bind overhead; target missed despite the legacy pass flag. Seed content matches the official hash.

WSL mirrored to NAT, same 5k TCP probe, three runs each: median connection setup 2.697 to 0.103 s; source bind 503.7 to 1.21 us/call. 10k HTTP, 1-second ramp/15-second hold: activation 1.0007 s, hold p99 4.37 ms, zero errors. Late ramps now fail validation (100 ms scheduling margin). [Data](src/benchmarks/load-buildup.json).

WAL eviction, NAT 350k, one serial pair, seed 42, 60-second ramp/hold, limit 0 in both: hold p99 11.01 to 5.77 ms; peak RAM 1512.96 to 956.17 MiB; median hold file cache 500.58 to 253.00 MiB. Both reached 350k on time with zero errors or memory-limit events. [Data](src/benchmarks/wal-cache.json).

Statistics include the ramps. RAM is the cgroup peak, including charged file pages. Local results, not an official score or a measured capacity limit.

Rewrite diagnostics, five minutes: hold-only p99 3,160.56 ms. At 251-281 s, 4,314,459 receive ENOBUFS completions, CQ depth 2,048 and send-to-CQE service up to 700.63 ms; COMMIT max 12.91 ms. Timing overhead included. Full logs: `results/0a3c419-sync-300000-5m/`.

Receive retries, 1,536 clients with all 1,024 buffers held, 16 turns: control CQEs 4,096 to 32 (1.66 to 64.75 ms wall, intentional backoff). One-refill recovery and shutdown passed. The 50k pair ran serially, 5 s ramp / 15 s hold, different random seeds; no clear ordinary-request performance change.

The 70k submission/write runs used 5 s ramp / 15 s hold, serially with different random seeds. Early submission gave no gain and was reverted.

Bounded retries + prepared writes, 300k/5m: hold-only p99 7.36 ms. Release build without telemetry, original generator; the timer-budget change was not included.

Bounded generator timers, 300k/5m: hold-only p99 6.45 ms. Same server binary and CQ 2,048, new generator, different random seed; no internal telemetry.

CQ 8,192, 300k/5m: hold-only p99 6.69 ms. Same generator and work budgets; no clear steady-load gain versus CQ 2,048.

Primary defer, 70k: one serial pair, 5 s ramp / 15 s hold, different random seeds, no telemetry. Excludes the later CQE cursor and ring-span changes. Those reduced `next`/`wait` by 32/22 code bytes; two reversed native pairs gave mixed timings. [Data](src/benchmarks/primary-defer.json).

Primary defer, 300k/5m: includes the fixed CQE cursor and ring span. Hold-only p99 6.34 ms; no telemetry, memory-limit events or OOM kills.

Retry-only poll-first, four quiet pairs: recovery CPU 2.822 to 2.588 us/receive, p50 5.494 to 5.393 ms, retry submissions +0.2%. Forced ENOBUFS recovery, not HTTP throughput. [Data](src/benchmarks/recovery-reuse.json).

Warm response allocations/request: feed 1 to 0, post 3 to 0, missing post 1 to 0, write reply 2 to 0. Retained strings 290 to 6,419 bytes within the 8,192-byte Exchange cap. No additional read CPU gain over reserve-only established. [Data](src/benchmarks/recovery-reuse.json).

350k pair: no telemetry, different random seeds; native probes overlapped the reference. Hold-only p99 123.79 to 6.97 ms. One local pair does not isolate the cause of the tail difference.

400k/5m: same server and generator as 350k, no telemetry or parallel benchmarks. Hold-only p99 7.68 ms; no memory-limit events or OOM kills.

unix-excl, 70k: one serial pair, 5 s ramp / 15 s hold, different random seeds, no telemetry. CPU/request fell 2.5%; p99 was nearly unchanged. The separate SQLite probe reduced measured fcntl64 calls from 67,080 to zero, excluding startup.

Prepared Store controls, 20k writes, two reversed pairs: median CPU/write at batch 1/4/16 was 38.98/19.34/9.74 to 38.00/19.16/8.56 us. Outputs and stored counts matched; final checkpoint included, wall time variable. [Data](src/benchmarks/group-commit.json).

Fused like INSERT, 20k writes, two reversed pairs per SQL variant: at batch 16, fresh likes were 5-6% slower, duplicates 13-16% slower and mixed writes 7-9% slower. Outcomes and stored checksums matched. Reject the SELECT forms. [Data](src/benchmarks/group-commit.json).

Direct like lookup, 20k writes, two reversed pairs: VALUES plus B-tree fallback reduced batch-16 CPU/write from 4.94 to 4.73 us (fresh), 1.09 to 0.98 us (duplicate), 4.36 to 4.20 us (mixed). Batch-1 mixed was 1.8% slower. Outcomes and stored checksums matched; foreign keys stayed enabled. [Data](src/benchmarks/group-commit.json).

unix-excl + direct like lookup, 350k/5m: hold p99 5.69 ms, no request errors. Combined confirmation without internal telemetry; the isolated Like comparison is the native probe above.

Create without RETURNING, 20k writes, two reversed pairs: CPU/write at batch 1/16 fell from 36.01/7.10 to 31.71/6.03 us. Every returned ID and timestamp matched storage. Store lifecycle and checkpoint worker included. [Data](src/benchmarks/group-commit.json).

Create readback, 350k/5m: no clear overall CPU or latency change. Post-drain memory rose 358.85 MiB, 96.4% in file cache; the physical WAL was 336.92 MiB larger. No memory-limit or OOM events. Different seeds; checkpoint lag versus retained WAL capacity remains unresolved.

CQ burst, 4,096 completions, two reversed pairs: median drain CPU 0.216/0.023/0.021 ms at CQ 2,048/8,192/16,384. SQ 1,024, 256-CQE drains, no loss; larger queues avoided overflow. Kernel microbenchmark, not HTTP throughput. [Data](src/benchmarks/cq-sizing.json).

Admission comparison: 5 s ramp, 15 s hold, 30 s down. One pair with different random seeds.

300k feed batch, five minutes: hold-only p99 1,172.77 ms; aggregate p99 749.36 ms.

300k group commit, five minutes, no internal telemetry: hold-only p99 97.99 ms; maximum 3,954.01 ms. The short run had worse p99 than the earlier short feed profile.

300k batch cleanup, five minutes, no internal telemetry: hold-only p99 7.04 ms; maximum 159.68 ms.

300k typed pipeline, five minutes, no internal telemetry: hold-only p99 6.54 ms; maximum 86.89 ms.

500k: no completed requests during hold; server CPU fell while the client used one core. Cause unresolved; this run does not establish server capacity.

Normal repeat: same server/client/seed/workload hashes, different RNG seed; p99 rose from 749.36 to 1,702.04 ms. No memory-limit or reclaim events.

Diagnostics: foreground COMMIT maxima 2,526.89 and 1,426.01 ms; full CQs and CPU saturation afterward. Client timer p99 about 1.1 ms, epoll p99 below 0.25 ms. Memory limit reached during hold, no OOM. [Data](src/profiles/stalls-300000.json).

Low-hanging and direct-descriptor pairs ran one baseline and one candidate concurrently. Direct-descriptor pairs used the official 65,535 file limit.

Transport check: 70,000 open connections with `RLIMIT_NOFILE=65535`, two rings, 1 CPU and 2 GiB. Slot reuse and shutdown passed.

The uring rows read CPU counters externally; earlier rows include the in-container counter reader. Their CPU/request values are not directly comparable.

[Data](src/benchmarks/results.json). [300k CPU profile](src/profiles/peak-cpu-300000.json). Full logs stay in `results/<run>/`.

Feed batching: native median 10.58 to 9.06 us over five alternating pairs; after cleanup, 10.42 to 9.05 us over three pairs. Outputs matched. [Data](src/benchmarks/feed-batch.json).

Leaf-local likes, five alternating native pairs, 10k feeds/sample: median thread CPU 6.54 to 6.01 us on seed data (-8.2%), 31.57 to 12.99 us with dense likes (-58.9%), 8.03 to 6.42 us with sparse feed IDs (-20.1%). JSON bytes matched. [Data](src/benchmarks/leaf-likes.json).

Feed initialization, five native pairs, 30k feeds/sample: both changes gave paired median CPU +0.8% seed, +3.2% dense, +1.6% sparse. Close-only: -0.1% / +0.2% / +1.9%. Reverted both; outputs matched. [Data](src/benchmarks/feed-init.json).

Group commit, 10,000 valid writes, two reversed pairs: b=1 to b=16 reduced median CPU/write from 63.85 to 51.88 us, WAL frames from 53,424 to 41,488, commits from 10,000 to 625. Outputs/state matched; inline checkpoints, no HTTP timing. b=64 saved more CPU in the mixed test but showed unstable wall time. [Data](src/benchmarks/group-commit.json).

Batch cleanup: `flush_writes` code 3,966 to 2,570 bytes, reserved stack 984 to 184 bytes; total executable text 3,332 bytes smaller.

`SQLITE_DEFAULT_MEMSTATUS=0`, seven alternating CPU comparisons: feed paired median 1.0% slower, post 0.9% faster. All JSON hashes matched.

Release LTO, seven alternating CPU comparisons: feed medians 10.358 to 10.227 us, post 4.086 to 4.169 us; executable text 10.1% smaller. All JSON hashes matched.

`-mtune=cascadelake`, 7 pairs: feed 10.218 to 10.231 us; post 4.088 to 4.041 us.

GCC13 to GCC16 (C++23/GNU17), 7 pairs: feed 10.147 to 10.279 us; post 4.081 to 4.127 us. GCC16 needed a SQLite warning exception.

SQLite page cache, 2 to 16 MiB, five alternating pairs: random-post CPU median change -12.6%, misses -37.3%, feed unchanged. JSON hashes matched. Mixed 10k profile: 51.85 us/request, no errors, no clear overall CPU change. [Data](src/profiles/cache16-10000.json).

Load generator, 10k users / 60 s: sampled client cgroup peak 2,775.08 MiB (k6) versus 52.63 MiB (native); 795.95 versus 797.57 requests/s, no errors. Native process peak RSS: 24.05 MiB. [Data](src/profiles/cpp-load-10000.json).

Accept depth 1 to 64, 512-connection bursts behind a 20-ms handler gate, file limit 256: actual accepts during the gate 1 to 64; after-release p99 10.223 to 6.106 ms and 9.011 to 6.281 ms. All responses passed. [Data](src/benchmarks/accept-depth.json).

Parser reuse, 100k lifecycle cycles, four alternating samples: JWT/body CPU median 29.098 to 20.631 ms, allocations 250,009 to 16. With 25% oversized bodies: 30.288 to 27.834 ms, allocations 250,009 to 275,006 as oversized Exchanges are discarded. Checksums matched. Reset code: 1,335 to 267 bytes.

Header passing, no SQLite, seven confirmation pairs: `AcceptedHead&&` feed -4.86%, post +1.52%, mixed -3.20% (4/7 faster), protected +2.42%. Kept value parameters. [Data](src/benchmarks/headers.json).

## Mini PC, direct Ethernet

Ryzen 7 3750H; server: 1 CPU, 2 GiB. PC generator over 2.5 GbE, 32 destination ports, 70k users, 15-second hold.

| WSL mode | Ramp | Connections | Requests | p95 | p99 | Errors |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| NAT | 5 s | 64,542/70,000 | 171,678 | 740.17 ms | 2,283.10 ms | 0 |
| Mirrored | 5 s | 70,000/70,000 | 178,305 | 3,557.69 ms | 6,922.62 ms | 0 |
| Mirrored | 30 s | 70,000/70,000 | 292,572 | 5.21 ms | 6.66 ms | 0 |

With the 30-second ramp, all users received a response; hold p99 was 6.79 ms, maximum 20.65 ms. Same server and generator binaries in all three runs.

Ubuntu 22.04 VM, direct LAN, one destination port and 64 source IPs: 70k users, 30-second ramp, 15-second hold, 293,009 requests, zero errors. p95 4.78 ms, p99 5.98 ms, hold p99 5.76 ms; server peak 761.91 MiB. All 70k connections and source IPs verified on the Mini.

200k users, 90-second ramp, 60-second hold, seed 42. All users received responses; zero errors.

| Filesystem | WAL policy | Requests | Hold p99 | Peak RAM | Physical reads |
| --- | --- | ---: | ---: | ---: | ---: |
| Btrfs | Keep | 2,548,997 | 47.90 ms | 2071.98 MiB | 0.00 MiB |
| Btrfs | Evict | 2,548,715 | 72.77 ms | 1296.30 MiB | 989.18 MiB |
| ext4 | Keep | 2,549,732 | 6.79 ms | 1590.00 MiB | 0.00 MiB |
| ext4 | Evict | 2,548,293 | 74.61 ms | 1352.79 MiB | 1475.67 MiB |
| ext4 | Evict + journal_size_limit=0 | 2,548,677 | 7.81 ms | 1322.78 MiB | 3.56 MiB |

Ext4 runs preloaded only the immutable source outside the server cgroup; all three fresh databases started at 237.65 MiB file cache inside the limit. One run per variant. [WAL data](src/benchmarks/wal-cache.json).

## Mini PC, Linux loopback

Server: 1 CPU, 2 GiB. Generator on the same Mini, one destination port, 64 source IPs, 90-second ramp, 60-second hold.

| Users | Requests | Hold requests/s | p95 | p99 | Max | Errors | Peak RAM |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 200,000 | 2,548,541 | 21,163.90 | 5.82 ms | 13.36 ms | 185.03 ms | 0 | 2,067.75 MiB |

All users received responses; hold p99 was 20.43 ms. Client teardown triggered 26,113 memory.max events and page reclaim, with no OOM kills. Peak briefly exceeded the configured 2 GiB.

Diagnostic repeat, same seed and workload: 2,548,756 requests, zero errors, hold p99 8.41 ms. WAL stayed at 969.03 MiB after 31 observed resets; the final 1,042 logical frames were fully checkpointed. Peak RAM 2,070.32 MiB; 38,977 memory.max events, no OOM kills.

WAL cache eviction, Mini 50k, two reversed pairs, 20-second holds: mean file cache after drain 252.48 to 243.39 MiB; mean hold p99 5.861 to 5.901 ms, zero errors. Peak RAM varied. On Mini and Docker, a residency probe reduced 64.54 MiB of WAL cache to 8 KiB, with rc=0 and fresh reads intact. [Data](src/benchmarks/wal-cache.json).

Docker HTTP comparison remains inconclusive: one run missed target concurrency before the first checkpoint; a diagnostic repeat passed. Both showed substantial I/O wait.

## Native CPU sampling

unix-excl, 350k with the same 30-second warm-up and 90-second capture: CPU/request 24.19 to 21.99 us; hold p99 7.10 to 6.40 ms. No fcntl samples (previously 7.5% of CPU). Network 46.0%, SQL/rows 20.3%, io_uring 11.0%. 65,439 samples, no loss or request errors. Different random seeds. [Data](src/profiles/unix-excl-350000.json).

350k, 30-second warm-up and 90-second capture: 73,713 samples, no loss, 3,143 unresolved leaves. Network 40.7%, SQL/rows 20.9%, files/locks/page cache 12.8%, io_uring 8.6%, response JSON 2.6%. No request errors; hold p99 7.10 ms. Sampling overhead included. [Data](src/profiles/recovery-reuse-350000.json).

300k backlog, 30-second warm-up and 90-second capture: 80,484 samples, no loss; network 38.7%, io_uring 28.6%, SQL 11.6%. [Data](src/profiles/admission-backlog-300000.json).

Rewrite versus `671fd4e`, 10k users, 30-second warm-up and 60-second capture: hold CPU 47.42 to 45.71 us/request, p99 4.341 to 4.310 ms; aggregate CPU 53.01 to 54.22 us/request. No errors or lost samples. One pair, different random seeds. [Data](src/profiles/architecture-reference-10000.json).

50,000 users before the B-tree change, 30-second ramp and 60-second capture. User CPU: SQL/rows 68.5%, transport/other 21.9%, response JSON 4.9%, auth 4.6%.

5,443 samples, no loss, 775 unresolved leaves. Hold CPU: 5.915 s user, 9.306 s system.

[Data](src/profiles/native-50000.json). Raw capture: `results/native-current-50000/`.

Kernel capture after the B-tree change, same load and window: network/socket paths 57.7%, io_uring 19.1%, files/locks/page cache 15.6%. Attribution follows stack paths. By syscall, including nested work: io_uring_enter 76.1%, fcntl/unixShmLock 10.6%, pwrite64 4.5%, fsync 2.2%, pread64 1.0%. 8,017 samples, no loss, two unresolved leaves. Hold CPU: 4.963 s user, 8.784 s system. No request errors. Loopback includes peer/ACK work in server context; WAL versus checkpoint writes remain unresolved.

[Data](src/profiles/kernel-50000.json). Raw capture: `results/kernel-btree-50000/`.

Current combined capture, same load and window: network 38.2%, SQL/rows 21.8%, io_uring 12.8%, files/locks/page cache 10.9%, JSON 2.1%, auth 2.0% of sampled CPU. 10,549 samples, no loss, 435 unresolved leaves. No request errors. [Data](src/profiles/combined-50000.json).

Compact transport, 10,000 users, 30-second ramp and 60-second capture: network 37.1%, SQL/rows 20.4%, io_uring 17.9%, files/locks/page cache 9.7%. Leaf CPU: interrupt unlock 10.8%, `finish_task_switch` 8.4%. 2,657 samples, no loss, 101 unresolved leaves, no request errors. [Data](src/profiles/compact-10000.json).

## Request-path sampling

Python 3.15.2, 5,000 users, five-minute hold. External CPU/wall samplers at 1 kHz on Granian's Python request thread. Unchanged API. The load run passed: 186,776 requests, p95 1.056 ms, p99 1.616 ms, no errors.

| Within captured handler stacks | CPU samples | Wall samples |
| --- | ---: | ---: |
| SQL + database wrapper | 49.5% | 52.7% |
| Row materialization | 0.1% | 0.1% |
| Response JSON | 31.2% | 24.6% |
| Auth, including JWT JSON | 3.2% | 2.6% |
| Routing, response handoff and other | 16.0% | 20.0% |

5,842 handler CPU samples and 8,521 handler wall samples. Another 59.8% of valid CPU samples were outside the handler, mainly asyncio/event-loop work. Native HTTP threads and suspended async waits are outside this profile's scope.

These are sampling estimates, not exact function timings. Stack-read failures: CPU 2.58%, wall 2.44%. Body-JSON parsing and GC had no captured samples; that does not establish zero cost.

Raw samples and flamegraphs: `results/request-profile-5000/`.
