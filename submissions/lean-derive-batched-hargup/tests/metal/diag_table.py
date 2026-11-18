#!/usr/bin/env python3
"""diag_table.py RESULT_DIR... — per-request costs from the perf stat window of bench153 runs made
with --syscalls-at (diagnostic runs: tracepoints perturb timing, so these are not CPU comparisons).
Counts cover all threads of the server (event loop and checkpoint thread)."""
import json, sys
keys = [('raw_syscalls:sys_enter', 'syscalls'), ('syscalls:sys_enter_io_uring_enter', 'enter'),
        ('syscalls:sys_enter_pwrite64', 'pwrite'), ('syscalls:sys_enter_pread64', 'pread'),
        ('syscalls:sys_enter_nanosleep', 'nanosleep'), ('syscalls:sys_enter_clock_nanosleep', 'clock_ns'),
        ('syscalls:sys_enter_epoll_wait', 'epoll_wait'), ('syscalls:sys_enter_futex', 'futex'),
        ('syscalls:sys_enter_fdatasync', 'fdatasync'), ('syscalls:sys_enter_fsync', 'fsync'),
        ('syscalls:sys_enter_fcntl', 'fcntl'), ('syscalls:sys_enter_newfstat', 'fstat'), ('syscalls:sys_enter_lseek', 'lseek')]
print('| variant | users | ' + ' | '.join(k for _, k in keys) + ' | other | kcycles u / k | kinstr u / k | IPC u / k | branch-miss | cache-miss |')
print('|---|---:|' + '---:|' * (len(keys) + 6))
for d in sys.argv[1:]:
    s = json.load(open(d.rstrip('/') + '/summary.json'))
    sc = s.get('syscalls')
    if not sc or not sc.get('per_request'):
        print(f"| {s['variant']} | (no counters) |"); continue
    pr = sc['per_request']
    c = {k: pr.get(k, 0) for k, _ in keys}
    other = c['raw_syscalls:sys_enter'] - sum(v for k, v in c.items() if k != 'raw_syscalls:sys_enter')
    g = lambda k: pr.get(k, 0)
    cu, ck, iu, ik = g('cycles:u'), g('cycles:k'), g('instructions:u'), g('instructions:k')
    print(f"| {s['variant']} | {s['vus']:,} | " + ' | '.join(f'{c[k]:.3f}' for k, _ in keys) +
          f" | {other:.3f} | {cu/1e3:.1f} / {ck/1e3:.1f} | {iu/1e3:.1f} / {ik/1e3:.1f} | "
          f"{(iu/cu if cu else 0):.2f} / {(ik/ck if ck else 0):.2f} | {g('branch-misses'):.0f} | {g('cache-misses'):.0f} |")
