import pathlib, time, sys
quiet=0
goal=int(sys.argv[1]) if len(sys.argv)>1 else 10
while quiet<goal:
    busy=[]
    for p in pathlib.Path('/proc').glob('[0-9]*/cmdline'):
        try:
            argv=p.read_bytes().split(b'\0')
        except OSError:
            continue
        if argv[0].rsplit(b'/',1)[-1] in (b'loadgen', b'blast', b'cc1'):
            busy.append(p.parent.name)
    quiet=0 if busy else quiet+1
    time.sleep(1)
print(f'{goal}-second quiet window established', flush=True)
