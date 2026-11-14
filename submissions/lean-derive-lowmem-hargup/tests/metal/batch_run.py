#!/usr/bin/env python3
"""Run the generated batch regression under ASAN/UBSAN as UID 65534."""
import json, os, pathlib, shutil, subprocess, sys, tempfile
root=pathlib.Path('/root/twelve/opt-audit-1024/metal')
sha=sys.argv[1]
with tempfile.TemporaryDirectory(prefix='metal-batch-') as tmp:
    work=pathlib.Path(tmp)
    shutil.copyfile('/root/twelve/challenge/seed/feed.db',work/'feed.db')
    shutil.copyfile(root/'artifacts'/f'{sha}-batch/server',work/'test')
    (work/'test').chmod(0o755)
    for p in [work,work/'feed.db',work/'test']:os.chown(p,65534,65534)
    token=json.loads(pathlib.Path('/root/twelve/challenge/seed/tokens.json').read_text())[0]['token']
    env=dict(os.environ,SQLITE_PATH=str(work/'feed.db'),BENCH_AUTH='Bearer '+token,ASAN_OPTIONS='detect_leaks=0')
    subprocess.run(['setpriv','--reuid=65534','--regid=65534','--clear-groups',str(work/'test')],env=env,check=True,timeout=60)
