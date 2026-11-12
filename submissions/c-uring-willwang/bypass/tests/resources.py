#!/usr/bin/env python3
"""Check bounded lwIP admission and SQLite failure on the scorer's private veth.

After preparing tests/bypass's fixtures and building bin/server-bypass, run
from the submission directory:

    python3 bypass/tests/resources.py --work /tmp/bypass-work --out /tmp/bypass-resource-result \
        --server-cpu 15 --client-cpu 14

Use a fresh output directory and two available CPU numbers. This test gives
the same unprivileged server a 16-connection cap and a gated WAL interposer.
The scored server and the regular transport tests do not load the interposer.
Add --xdp-hybrid to exercise shared-IP generic XDP receive and packet GSO transmit.
"""
import argparse
from contextlib import closing
import json
from pathlib import Path
import shlex
import sqlite3
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ENTRY = HERE.parents[1]
sys.path[:0] = [str(ENTRY / "tests/bypass"), str(ENTRY / "tests")]
import differential as fixture
from correctness import client_program
from lab import PORT, REPO, PrivateLink, application, digest, save, validate_cpus
import xdp_service


def run(args):
    manager = args.client_cpu if args.manager_cpu is None else args.manager_cpu
    validate_cpus(manager, args.server_cpu, [[args.client_cpu]])
    args.work = args.work.resolve(strict=True)
    binary = args.binary.resolve(strict=True)
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    args.client_cpus = str(args.client_cpu)
    args.mode = "xdp" if args.xdp_hybrid else "packet"
    if args.xdp_hybrid:
        args.packet_gso = args.shared_ip = args.rx_cpu_stats = True
        args.xdp_attachment = "skb"
    args.action = "resources"
    with tempfile.TemporaryDirectory(prefix="twelve-lwip-resource-") as directory:
        scratch = Path(directory)
        database = scratch / "seed.db"
        preload = scratch / "fail-wal.so"
        gate = scratch / "reject-wal-write"
        fixture.seed(database, "dense")
        subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        str(HERE / "fail_wal.c"), "-ldl", "-o", str(preload)], check=True)
        # exec keeps the service PID, resources and fault environment together.
        # The XDP bootstrap and ordinary Linux client run without the interposer.
        launcher = scratch / "private-server"
        start = application
        startup = {}
        if args.xdp_hybrid:
            args.binary = binary
            start = xdp_service.application
            subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                            str(HERE / "wal_launcher.c"), "-o", str(launcher)], check=True)
            startup = {"launcher": launcher,
                       "readonly_mounts": [(binary, "/actual-server"), (preload, "/qa-failwal.so")]}
        else:
            launcher.write_text(
                "#!/bin/sh\nexport LD_PRELOAD=" + shlex.quote(str(preload)) +
                "\nexport WIRE_FAIL_WAL=" + shlex.quote(str(gate)) +
                "\nexec " + shlex.quote(str(binary)) + ' --max-connections 16 "$@"\n')
            launcher.chmod(0o700)
            args.binary = launcher
        with PrivateLink(scratch, args.out, args.server_cpu, [[args.client_cpu]], 1,
                         args.work, REPO / "seed/tokens.json", manager) as link:
            with start(args, link, database, "service", fixture.SECRET, **startup) as service:
                before = link.server_counters()
                host, pid, _, _, out = service
                log = out / "transport.log"
                program = [sys.executable, "-B", str(HERE / "transport_qa.py"), "--host", host,
                           "--port", str(PORT), "--server-pid", str(pid), "--server-log", str(out / "server.log"),
                           "--loss", "--admission-limit", "16", "--wal-gate", str(gate), "--wal-barrier",
                           "--secret", fixture.SECRET]
                client_program(args, link, program, log, timeout=60)
                result = json.loads(log.read_text())
                after = link.server_counters()
                for counter in ("InSegs", "OutSegs", "CurrEstab"):
                    if before[counter] != after[counter]:
                        raise AssertionError(f"server-kernel TCP counter changed: {counter}")
                if result["stack"]["commit_failures"] != 1:
                    raise AssertionError("expected exactly one rejected SQLite commit")
            # application() kills the server with SIGKILL. Recover with a new
            # stock SQLite connection, independent of the server's read path.
            wal = result["wal"]
            with closing(sqlite3.connect(database)) as db:
                actual = db.execute("SELECT body FROM posts WHERE id=?", (wal["acknowledged_id"],)).fetchone()
                rejected = db.execute("SELECT id FROM posts WHERE body=?", (wal["rejected_body"],)).fetchone()
                if actual != (wal["acknowledged_body"],) or rejected is not None:
                    raise AssertionError("recovery lost an acknowledged post or exposed the failed post")
                if db.execute("PRAGMA quick_check").fetchall() != [("ok",)]:
                    raise AssertionError("SQLite's recovered database failed quick_check")
        result.update(reopened_after_sigkill=True, no_reply_while_wal_blocked=True,
                      server_kernel_tcp_before=before, server_kernel_tcp_after=after,
                      binary_sha256=digest(binary), packet_gso=args.packet_gso, mode=args.mode,
                      packet_tx_frames=args.packet_tx_frames, xdp_rx_frames=args.xdp_rx_frames if args.xdp_hybrid else None)
        save(args.out / "resources.json", result)
        print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--binary", type=Path, default=ENTRY / "bin/server-bypass")
    parser.add_argument("--server-cpu", type=int, required=True)
    parser.add_argument("--client-cpu", type=int, required=True)
    parser.add_argument("--manager-cpu", type=int, help="private namespace setup CPU, defaults to the client CPU")
    parser.add_argument("--memory-mib", type=int, default=1730)
    parser.add_argument("--packet-gso", action="store_true", help="exercise the optional queued segmentation path")
    parser.add_argument("--xdp-hybrid", action="store_true", help="use shared-IP generic AF_XDP RX and AF_PACKET GSO TX")
    parser.add_argument("--xdp-rx-frames", type=int, default=512, help="AF_XDP receive slots for the hybrid fixture")
    parser.add_argument("--packet-tx-frames", type=int, help="AF_PACKET transmit ring slots")
    run(parser.parse_args())
