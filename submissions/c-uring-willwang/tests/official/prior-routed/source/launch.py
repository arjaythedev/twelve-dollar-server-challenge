"""Final unprivileged launch in the intended namespace and (for servers) the measured scope."""
import json
import os
from pathlib import Path
import resource
import shutil
import sys

role, pidfile, cpus, files, *args = sys.argv[1:]
pins = {int(c) for c in cpus.split(",")}
want = int(files)
os.sched_setaffinity(0, pins)
limit = resource.RLIMIT_NOFILE
soft, hard = resource.getrlimit(limit)
resource.setrlimit(limit, (want if role == "server" else max(soft,want), want if role=="server" else hard))
if role=="server":
    seed, dest, *args=args
    shutil.copyfile(seed, dest)
made=Path(pidfile)
info = dict(pid=os.getpid(), cpus=sorted(os.sched_getaffinity(0)),
    nofile=list(resource.getrlimit(limit)), ns=os.stat("/proc/self/ns/net").st_ino,
    cgroup=Path("/proc/self/cgroup").read_text().strip(), uid=os.getuid())
made.with_suffix(".settings.json").write_text(json.dumps(info))
made.write_text(str(os.getpid()))
os.execv(args[0],args)
