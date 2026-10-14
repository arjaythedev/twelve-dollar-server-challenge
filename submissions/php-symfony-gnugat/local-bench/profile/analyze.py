"""Summarizes Excimer collapsed stacks: python3 -I analyze.py <dir-of-txt-files> [frame-substring...]

Prints the share of PHP CPU samples spent in (inclusive) frames matching each substring, then the top self-time
frames and the top inclusive frames under HttpKernel::handle.
"""
import collections
import pathlib
import sys

stacks = collections.Counter()
for path in pathlib.Path(sys.argv[1]).glob('*.txt'):
    for line in path.read_text().splitlines():
        stack, _, count = line.rpartition(' ')
        if stack:
            stacks[stack] += int(count)
total = sum(stacks.values())
print(f'{total} samples')


def short(frame):
    frame = frame.replace('Symfony\\Component\\', 'S\\').replace('Symfony\\Bundle\\', 'SB\\').replace('Doctrine\\', 'D\\')
    return frame if len(frame) < 110 else frame[:107] + '...'


def inclusive(predicate):
    return sum(n for stack, n in stacks.items() if any(predicate(f) for f in stack.split(';')))


for needle in sys.argv[2:]:
    print(f'{100 * inclusive(lambda f: needle in f) / total:5.1f}%  {needle}')

self_time = collections.Counter()
incl = collections.Counter()
for stack, n in stacks.items():
    frames = stack.split(';')
    self_time[frames[-1]] += n
    for frame in set(frames):
        incl[frame] += n

print('\n-- top self time')
for frame, n in self_time.most_common(25):
    print(f'{100 * n / total:5.1f}%  {short(frame)}')
print('\n-- top inclusive time')
for frame, n in incl.most_common(60):
    print(f'{100 * n / total:5.1f}%  {short(frame)}')
