"""Run the organizer's goldens and the shipped SQL oracles through actual Linux client TCP."""

import argparse
from contextlib import closing
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import differential as ref
from lab import PORT, REPO, application, save


def client_program(args, link, program, output, timeout=60):
    with output.open("w") as log:
        proc = subprocess.Popen(
            ["taskset", "-c", args.client_cpus, *program],
            env=link.client_env(),
            stdout=log,
            stderr=log,
            start_new_session=True,
        )
        try:
            code = proc.wait(timeout)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
            raise
    if code:
        raise RuntimeError(f"client exit {code}, {output}: {output.read_text()[-2300:]}")


def check_chunked(target):
    token = ref.token()
    request = (
        f"POST /posts HTTP/1.1\r\nHost: example\r\nAuthorization: Bearer {token}\r\n"
        "Transfer-Encoding: chunked\r\nContent-Type: application/json\r\n\r\n"
    ).encode()
    value = b'{"body":"chunk boundaries survive"}'
    chunks = [value[:1], value[1:9], value[9:]]
    with socket.create_connection((target, PORT), 3) as s:
        s.sendall(request)
        for chunk in chunks:
            s.sendall(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
        s.sendall(b"0\r\nX-Client: ordinary\r\n\r\n")
        response = http.client.HTTPResponse(s)
        response.begin()
        assert (
            response.status == 201
            and json.loads(response.read())["post"]["body"] == "chunk boundaries survive"
        )
    print("fragmented chunked request with a trailer persisted successfully")


def reference_client(args):
    with closing(http.client.HTTPConnection(args.host, PORT, timeout=5)) as conn:
        feed, posts, sample = json.loads(args.reference.read_text())
        reference = feed, {int(key): value for key, value in posts.items()}, sample
        ref.check_reads(conn, reference)
        if args.case == "short":
            ref.check_pipeline(conn, reference[0])
            check_chunked(args.host)
        if args.case == "dense" and not args.recover:
            save(args.ack, ref.write_and_check(conn, reference[0]))
    print("reference", args.case, "after crash" if args.recover else "before crash")


def check(args, link):
    records = []
    with tempfile.TemporaryDirectory(dir=link.root) as tmp:
        scratch = Path(tmp)
        if args.action == "golden":
            with application(
                args, link, scratch / "golden.db", "golden", "twelve-dollar-challenge", args.seed
            ) as api:
                host, pid, _, _, out = api
                client_program(
                    args,
                    link,
                    ["bash", str(REPO / "test/test.sh"), f"http://{host}:{PORT}"],
                    out / "golden.log",
                )
                records.append(
                    {"case": "organizer", "output": (out / "golden.log").read_text().splitlines()[-1]}
                )
                script = HERE.parents[1] / "bypass/tests/transport_qa.py"
                if script.is_file():
                    command = [sys.executable, str(script), "--host", host, "--port", str(PORT)]
                    if args.mode != "baseline":
                        command += [
                            "--server-pid",
                            str(pid),
                            "--server-log",
                            str(out / "server.log"),
                            "--loss",
                        ]
                    client_program(args, link, command, out / "transport.log", timeout=90)
                    records.append({"case": "transport", "output": (out / "transport.log").read_text()})
            if args.mode == "xdp" and args.shared_ip:
                from local_http import check as local_check

                with application(args, link, scratch / "localhost.db", "localhost",
                                 "twelve-dollar-challenge", args.seed) as api:
                    host, _, _, _, out = api
                    record = local_check(args, link, host, out)
                with closing(sqlite3.connect((scratch / "localhost.db").as_uri() + "?mode=ro", uri=True)) as db:
                    for key, body in (("local_read_remote_post", "remote write visible on localhost"),
                                      ("chunked_local_post", "chunked localhost write")):
                        if db.execute("SELECT body FROM posts WHERE id=?", (record[key],)).fetchone() != (body,):
                            raise RuntimeError("acknowledged post missing from independent SQLite connection")
                    likes = db.execute("SELECT count(*) FROM likes WHERE post_id=?",
                                       (record["local_read_remote_post"],)).fetchone()[0]
                    if likes != 1:
                        raise RuntimeError("acknowledged local like missing from independent SQLite connection")
                record["durable_posts"] = 2
                record["durable_likes"] = 1
                save(out / "local-and-remote.json", record)
                records.append({"case": "localhost and remote", **record})
        else:
            for case in ref.CASES:
                db = scratch / (case + ".db")
                ref.seed(db, case)
                ack = scratch / (case + "-acks.json")
                for recover in [False, True] if case == "dense" else [False]:
                    expected = ref.load_reference(db)
                    if recover:
                        rows = expected[1]
                        for item in json.loads(ack.read_text()):
                            ref.equal(rows.get(item["id"]), item, "durable previously acknowledged POST")
                    name = case + ("-recover" if recover else "")
                    oracle = scratch / (name + "-reference.json")
                    save(oracle, expected)
                    with application(args, link, db, name, ref.SECRET) as api:
                        host, _, _, _, out = api
                        command = [
                            sys.executable,
                            str(Path(__file__).resolve()),
                            "--host",
                            host,
                            "--reference",
                            str(oracle),
                            "--case",
                            case,
                            "--ack",
                            str(ack),
                        ]
                        if recover:
                            command += ["--recover"]
                        client_program(args, link, command, out / "reference.log")
                records.append({"case": case, "passed": True})
    save(args.out / "correctness.json", records)
    print(json.dumps(records), flush=True)


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--host", required=True)
    p.add_argument("--reference", type=Path, required=True)
    p.add_argument("--case", choices=ref.CASES, required=True)
    p.add_argument("--ack", type=Path, required=True)
    p.add_argument("--recover", action="store_true")
    reference_client(p.parse_args())
