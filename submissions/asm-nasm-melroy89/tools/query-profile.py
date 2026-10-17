#!/usr/bin/env python3
"""Query-only SQLite diagnostics, not HTTP/JSON serialization benchmarks."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--object', type=Path, required=True, help='existing measured SQLite object; no library rebuild')
p.add_argument('--iterations', type=int, default=10)
p.add_argument('--label', default='query-profile')
a = p.parse_args()
here = Path(__file__).resolve().parents[1]
if not a.label.replace('-', '').replace('_', '').isalnum(): p.error('invalid label')
if not 1 <= a.iterations <= 10000: p.error('invalid iterations')
obj = a.object.resolve()
subprocess.run(['nasm', '-f', 'elf64', str(here / 'tools/query-profile.asm'), '-o', str(here / 'build/query-profile.o')], check=True)
subprocess.run(['gcc', '-no-pie', '-flto', str(here / 'build/query-profile.o'), str(obj), '-lm', '-o', str(here / 'build/query-profile')], check=True)
sql = dict(re.findall(r'^(q_feed\w*): db `(.+)`,0$', (here / 'src/sql.inc').read_text(), re.M))
sql = {name: query.replace(chr(92) + chr(34), chr(34)) for name, query in sql.items()}
for name in ('q_feed_selected_json', 'q_feed_selected_plain'):
    sql[name + '_forced'] = sql[name].replace('FROM selected s JOIN likes l ON l.post_id=s.id', 'FROM selected s CROSS JOIN likes l WHERE l.post_id=s.id')
rows = []
for name, query in sql.items():
    result = subprocess.run([str(here / 'build/query-profile'), str(here.parents[1] / 'seed/feed.db'), query, str(a.iterations)], check=True, capture_output=True, text=True, timeout=120)
    row = dict(query=name, sql=query, **json.loads(result.stdout))
    rows.append(row)
    print(json.dumps(row), flush=True)
record = dict(kind='read-only query-only diagnostic; counters enabled, no application serialization',
              sqlite_object_sha256=hashlib.sha256(obj.read_bytes()).hexdigest(),
              profiler_sha256=hashlib.sha256((here / 'build/query-profile').read_bytes()).hexdigest(),
              database_sha256=hashlib.sha256((here.parents[1] / 'seed/feed.db').read_bytes()).hexdigest(),
              settings='mmap_size=1073741824; cache_size=500; temp_store=MEMORY', rows=rows)
(here / 'results' / (a.label + '.json')).write_text(json.dumps(record, indent=2) + '\n')
