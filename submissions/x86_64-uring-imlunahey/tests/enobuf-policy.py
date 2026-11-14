#!/usr/bin/env python3
"""Exercise the real NASM drain logic against deterministic fake ring state.

Run build.sh first. System SQLite only resolves unused database symbols in this
test executable; the tested function never opens a database or issues a syscall.
The shipped server continues to use its pinned source-built SQLite.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
exports = ['uring_enobuf_drain', 'uring_recv', 'ring_contexts', 'connections', 'enobuf_queue',
           'generations', 'enobuf_fair']
subprocess.run(['objcopy', '--redefine-sym', 'main=server_main',
                *['--globalize-symbol='+name for name in exports],
                'build/server.o', 'build/server-policy.o'], cwd=root, check=True)
subprocess.run(['gcc', '-O2', '-no-pie', '-pthread', 'tests/enobuf-policy.c',
                'build/server-policy.o', 'build/json.o', 'build/crypto.o',
                '-lsqlite3', '-lm', '-o', 'build/enobuf-policy'], cwd=root, check=True)
subprocess.run(['build/enobuf-policy'], cwd=root, check=True)
