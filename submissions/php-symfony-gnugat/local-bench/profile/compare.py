"""Compares two Excimer runs under the same load: python3 -I compare.py <before-dir> <after-dir> <frame-substring>

Each directory holds stacks.txt and k6.txt (bench.sh's .k6 output). Each sample under the frame goes to the first
bucket it matches, in order. Samples are absolute counts (0.1 ms of PHP CPU each, or of wall-clock time with
EXCIMER_EVENT=real), comparable because both runs got the same users for the same time; "per 1,000 requests" divides
by k6's total.
"""
import collections
import pathlib
import re
import sys

buckets = [
    ('connect: open, pragmas, schema read', lambda s: any('DBAL\\Connection::connect' in f for f in s)),
    ('like_count: a COUNT query per post (extra lazy)', lambda s: any('getLikeCount' in f or 'PersistentCollection::count' in f for f in s)),
    ('SQLite: PDO prepare/execute/fetch/commit', lambda s: s[-1].startswith('Doctrine\\DBAL\\Driver\\PDO\\') or s[-1].startswith('PDO')),
    ('ORM hydration', lambda s: any('AbstractHydrator::' in f or 'UnitOfWork::createEntity' in f for f in s)),
    ('ORM DQL to SQL, query cache', lambda s: any('Query::getSqlExecutor' in f or 'Query::parse' in f or 'Query\\Parser' in f for f in s)),
    ('ORM, besides SQLite (find, flush, query building)', lambda s: any('Doctrine\\ORM\\' in f for f in s)),
    ('DBAL, besides SQLite (parameters, results)', lambda s: any('Doctrine\\DBAL\\' in f for f in s)),
    ('Serializer (normalize)', lambda s: any('Serializer\\' in f for f in s)),
    ('JsonStreamer (and its generated writers)', lambda s: any('JsonStreamer\\' in f or 'json-streamer' in f or 'json_streamer' in f for f in s)),
    ('JsonResponse, json_encode', lambda s: any('JsonResponse' in f or 'json_encode' in f for f in s)),
    ('controller, other', lambda s: True),
]


def load(directory, needle):
    shares = collections.Counter()
    total = everything = 0
    for line in (pathlib.Path(directory) / 'stacks.txt').read_text().splitlines():
        stack, _, count = line.rpartition(' ')
        if not stack:
            continue
        n = int(count)
        everything += n
        frames = stack.split(';')
        at = next((i for i, f in enumerate(frames) if needle in f), None)
        if at is None:
            continue
        total += n
        shares[next(name for name, matches in buckets if matches(frames[at:]))] += n
    requests = int(re.search(r'http_reqs\D+(\d+)', (pathlib.Path(directory) / 'k6.txt').read_text()).group(1))
    return shares, total, everything, requests


before, after = (load(d, sys.argv[3]) for d in sys.argv[1:3])
print(f'{sys.argv[3]}')
print(f'{"":50} {"before":>9} {"after":>9} {"change":>8}')
for label, b, a in [
    ('all PHP samples', before[2], after[2]),
    ('k6 requests', before[3], after[3]),
    ('samples under the frame', before[1], after[1]),
    ('  per 1,000 requests', 1000 * before[1] / before[3], 1000 * after[1] / after[3]),
]:
    print(f'{label:50} {b:9.0f} {a:9.0f} {100 * (a - b) / b:+7.0f}%')
print()
for name, _ in buckets:
    b, a = before[0][name], after[0][name]
    print(f'{name:50} {100 * b / before[1]:8.1f}% {100 * a / after[1]:8.1f}%   ({b} -> {a} samples)')
