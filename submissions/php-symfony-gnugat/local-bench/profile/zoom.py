"""Zooms into one frame of Excimer collapsed stacks: python3 -I zoom.py <dir-of-txt-files> <frame-substring> [depth]

Prints the inclusive and self time of the frames under the first frame matching the substring, as a share of that
frame's samples; depth limits how far below it frames are listed (default: all).
"""
import collections
import pathlib
import sys

needle = sys.argv[2]
depth = int(sys.argv[3]) if len(sys.argv) > 3 else 999
incl = collections.Counter()
self_time = collections.Counter()
total = 0
for path in pathlib.Path(sys.argv[1]).glob('*.txt'):
    for line in path.read_text().splitlines():
        stack, _, count = line.rpartition(' ')
        frames = stack.split(';')
        at = next((i for i, f in enumerate(frames) if needle in f), None)
        if at is None:
            continue
        n = int(count)
        total += n
        for frame in set(frames[at + 1:at + 1 + depth]):
            incl[frame] += n
        self_time[frames[-1] if len(frames) - at - 1 <= depth else '(deeper)'] += n
print(f'{total} samples under {needle}')


def short(frame):
    frame = frame.replace('Symfony\\Component\\', 'S\\').replace('Doctrine\\', 'D\\')
    return frame if len(frame) < 120 else frame[:117] + '...'


print('\n-- inclusive')
for frame, n in incl.most_common(45):
    print(f'{100 * n / total:5.1f}%  {short(frame)}')
print('\n-- self')
for frame, n in self_time.most_common(20):
    print(f'{100 * n / total:5.1f}%  {short(frame)}')
