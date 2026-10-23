"""What grew or shrank under a frame between two Excimer runs: python3 -I delta.py <before-dir> <after-dir> <frame-substring> [rows]

Prints each frame's inclusive samples (absolute, from stacks.txt) before, after and the difference, biggest change first.
"""
import collections
import pathlib
import sys


def load(directory):
    incl = collections.Counter()
    for line in (pathlib.Path(directory) / 'stacks.txt').read_text().splitlines():
        stack, _, count = line.rpartition(' ')
        frames = stack.split(';')
        at = next((i for i, f in enumerate(frames) if sys.argv[3] in f), None)
        if at is None:
            continue
        for frame in set(frames[at:]):
            incl[frame] += int(count)
    return incl


def short(frame):
    frame = frame.replace('Symfony\\Component\\', 'S\\').replace('Doctrine\\', 'D\\')
    return frame if len(frame) < 110 else frame[:107] + '...'


before, after = load(sys.argv[1]), load(sys.argv[2])
rows = int(sys.argv[4]) if len(sys.argv) > 4 else 25
for frame in sorted(set(before) | set(after), key=lambda f: -abs(after[f] - before[f]))[:rows]:
    print(f'{before[frame]:8} {after[frame]:8} {after[frame] - before[frame]:+8}  {short(frame)}')
