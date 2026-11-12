#!/usr/bin/env python3
"""Run store contracts in plain/diagnostic builds using disposable seed backups."""
from contextlib import closing
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

source = Path(__file__).resolve().parents[1]
seed = Path(sys.argv[1]).resolve()
flags = """-O1 -DSQLITE_DEBUG -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0
-DSQLITE_DQS=0 -DSQLITE_OMIT_LOAD_EXTENSION -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1
-DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_TRACE
-DSQLITE_OMIT_AUTOINIT""".split()


def run(args, **kwargs):
    result = subprocess.run(args, cwd=source, text=True, capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(f"{args}:\n{result.stdout}{result.stderr}")
    return result


def records(stderr):
    return [dict(item.split("=", 1) for item in line.split()[1:])
            for line in stderr.splitlines() if line.startswith("hkn_diag ")]


with tempfile.TemporaryDirectory(prefix="huykn-diagnostics-") as directory:
    work = Path(directory)
    cc = os.environ.get("CC", "cc")
    run([cc, *flags, "-c", "sqlite_engine.c", "-o", str(work / "sqlite.o")])
    run([cc, "-O1", "testdata/diagnostics_contract.c", "-o", str(work / "clock")])
    fixture = records(run([str(work / "clock")]).stderr)
    assert len([r for r in fixture if "wal_max" in r]) == 2, fixture

    for diagnostic in (False, True):
        binary = work / ("diagnostic" if diagnostic else "plain")
        run([cc, *flags, *(["-DHKN_DIAGNOSTICS"] if diagnostic else []),
             "testdata/store_contract.c", "checkpoint.c", str(work / "sqlite.o"),
             "-lm", "-lpthread", "-o", str(binary)])
        for mode in ("worker", "inline"):
            db = work / f"{binary.name}-{mode}.db"
            with closing(sqlite3.connect(seed.as_uri() + "?mode=ro", uri=True)) as original:
                with closing(sqlite3.connect(db)) as copy:
                    original.backup(copy)
            env = dict(os.environ, HKN_CHECKPOINT=mode)
            env.pop("HKN_SQL_READS", None)
            env.pop("HKN_VFS", None)
            result = run([str(binary), str(db)], env=env)
            rows = records(result.stderr)
            if diagnostic:
                main = next(r for r in rows if r["owner"] == "main" and "wal_max" in r)
                assert int(main["commit_errors"]) == 1, rows
                metrics = {r["metric"] for r in rows if "metric" in r}
                assert {"pass", "commit"} <= metrics, rows
                if mode == "worker":
                    worker = next(r for r in rows if r["owner"] == "worker" and "wal_max" in r)
                    assert int(worker["partial"]) >= 1, rows
                    assert int(main["wal_max"]) >= 65536, rows
                    assert {"worker_wait", "restart", "passive"} <= metrics, rows
                else:
                    assert metrics == {"pass", "commit"}, rows
            else:
                assert not rows, rows
            print(f"{binary.name}/{mode}: store contracts and diagnostics passed")
