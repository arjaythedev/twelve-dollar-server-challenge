"""Splits a write endpoint's Excimer samples into SQLite and Doctrine's parts: python3 -I split.py <dir> <frame-substring>

Each sample under the frame goes to the first bucket it matches, in order: a sample ending in PDO is SQLite's, even
inside hydration or flush().
"""
import collections
import pathlib
import sys

buckets = [
    ('SQLite: PDO commit/execute/fetch/prepare', lambda s: s[-1].startswith('Doctrine\\DBAL\\Driver\\PDO\\')),
    ('ORM hydration (hydrateAll)', lambda s: any('AbstractHydrator::hydrateAll' in f for f in s)),
    ('ORM fetch generated created_at', lambda s: any('fetchVersionAndNotUpsertableValues' in f for f in s)),
    ('ORM find/findOneBy, besides hydration', lambda s: any('EntityRepository::find' in f for f in s)),
    ('ORM flush, besides SQLite', lambda s: any('EntityManager::flush' in f for f in s)),
    ('ORM persist/getReference', lambda s: any('EntityManager::persist' in f or 'getReference' in f for f in s)),
    ('JSON response', lambda s: any('writeJson' in f for f in s)),
    ('controller, other', lambda s: True),
]
shares = collections.Counter()
total = 0
for path in pathlib.Path(sys.argv[1]).glob('*.txt'):
    for line in path.read_text().splitlines():
        stack, _, count = line.rpartition(' ')
        frames = stack.split(';')
        at = next((i for i, f in enumerate(frames) if sys.argv[2] in f), None)
        if at is None:
            continue
        n = int(count)
        total += n
        shares[next(name for name, matches in buckets if matches(frames[at:]))] += n
print(f'{total} samples under {sys.argv[2]}')
for name, _ in buckets:
    print(f'{100 * shares[name] / total:5.1f}%  {name}')
