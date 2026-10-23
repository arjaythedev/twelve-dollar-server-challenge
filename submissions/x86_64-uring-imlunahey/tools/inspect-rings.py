#!/usr/bin/env python3
"""Read-only, unsynchronized diagnostic snapshots; run in server PID namespace.

Requires SYS_PTRACE and an unstripped non-PIE binary's nm symbol map. Never
pauses the server. Extra instrumentation makes the result diagnostic, not a
replacement for an uninstrumented qualification.
"""
import argparse
import json
from pathlib import Path
import struct
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--symbols', required=True, type=json.loads)
p.add_argument('--output', required=True, type=Path)
a = p.parse_args()

def optional(path):
    try:
        return Path(path).read_text().strip()
    except (OSError, ValueError):
        return None

with open('/proc/1/mem', 'rb', buffering=0) as mem, a.output.open('w') as out:
    def number(address, code='Q'):
        mem.seek(address)
        return struct.unpack('<'+code, mem.read(struct.calcsize(code)))[0]

    def global_value(name, code='Q'):
        return number(a.symbols[name], code)

    while True:
        start = time.monotonic()
        try:
            row = dict(time=time.time(), monotonic=start, rings=[])
            for name in ('request_count', 'active_count', 'input_allocated', 'output_allocated'):
                row[name] = global_value(name)
            for name in ('wal_frames', 'wal_log', 'wal_checkpointed', 'checkpoint_requested'):
                row[name] = global_value(name, 'I')
            for i in range(global_value('uring_count', 'I')):
                base = a.symbols['ring_contexts'] + 512*i
                head = number(number(base+64), 'I')
                tail = number(number(base+72), 'I')
                row['rings'].append(dict(active=number(base+116, 'I'),
                    cq_depth=(tail-head) & 0xffffffff,
                    deferred=(number(base+132, 'I')-number(base+128, 'I')) & 0xffffffff))
            row['tasks'] = []
            for process in Path('/proc').iterdir():
                if not process.name.isdigit():
                    continue
                name = optional(process/'comm')
                if process.name != '1' and name != 'twelve-dollar-b':
                    continue
                for task in (process/'task').iterdir():
                    row['tasks'].append(dict(pid=int(process.name), tid=int(task.name),
                        name=optional(task/'comm'), schedstat=optional(task/'schedstat'),
                        wchan=optional(task/'wchan'), syscall=optional(task/'syscall')))
            out.write(json.dumps(row)+'\n')
            out.flush()
        except (OSError, struct.error):
            if not Path('/proc/1/exe').exists():
                break
        time.sleep(max(0, 1-(time.monotonic()-start)))
