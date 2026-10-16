import argparse
import json
import os
from pathlib import Path
import time

p = argparse.ArgumentParser()
p.add_argument('pid', type=int)
p.add_argument('output')
p.add_argument('--interval', type=float, default=1.0)
args = p.parse_args()
if not 0.5 <= args.interval <= 10:
    p.error('interval must be between 0.5 and 10 seconds')
proc = Path('/proc') / str(args.pid)
with open(args.output, 'w', buffering=1) as out:
    while proc.exists():
        try:
            status = dict(line.split(':', 1) for line in (proc/'status').read_text().splitlines() if ':' in line)
            mem = dict(line.split(':', 1) for line in Path('/proc/meminfo').read_text().splitlines())
            stat = (proc/'stat').read_text().split(') ', 1)[1].split()
            sample = {'time': time.time(), 'rss_kib': int(status['VmRSS'].split()[0]),
                      'peak_rss_kib': int(status['VmHWM'].split()[0]),
                      'process_ticks': int(stat[11])+int(stat[12]),
                      'guest_cpu_ticks': Path('/proc/stat').read_text().splitlines()[0],
                      'memory_available_kib': int(mem['MemAvailable'].split()[0]),
                      'memory_total_kib': int(mem['MemTotal'].split()[0]),
                      'socket_memory': Path('/proc/net/sockstat').read_text().splitlines(),
                      'threads': int(status['Threads']),
                      'fd_count': sum(1 for _ in os.scandir(proc/'fd'))}
            out.write(json.dumps(sample)+'\n')
        except (FileNotFoundError, ProcessLookupError):
            break
        time.sleep(args.interval)
