"""Optional observations after all stock Go clients close their TCP sockets."""

import ctypes
import json
import mmap
import os
import select
import socket
import sqlite3
import threading
import time
from pathlib import Path

from common import receive_namespace_fd, tcp_socket


MEMORY_FIELDS = frozenset(
    (
        "sock",
        "slab",
        "kernel",
        "anon",
        "file",
        "inactive_file",
        "active_file",
        "file_dirty",
        "file_writeback",
        "slab_reclaimable",
        "slab_unreclaimable",
        "pgscan",
        "pgsteal",
        "pgscan_direct",
        "pgsteal_direct",
        "pagetables",
        "kernel_stack",
    )
)


def resident_pages(path):
    """Count resident pages without faulting in the file contents or modifying them."""
    size_page = os.sysconf("SC_PAGE_SIZE")
    with path.open("rb") as f:
        size = os.fstat(f.fileno()).st_size
        pages = (size + size_page - 1) // size_page
        if not size:
            return {"bytes": 0, "pages": 0, "resident_pages": 0}
        with mmap.mmap(f.fileno(), size, access=mmap.ACCESS_COPY) as mapping:
            address = ctypes.addressof(ctypes.c_char.from_buffer(mapping))
            vec = (ctypes.c_ubyte * pages)()
            libc = ctypes.CDLL(None, use_errno=True)
            if libc.mincore(ctypes.c_void_p(address), ctypes.c_size_t(size), vec):
                raise OSError(ctypes.get_errno(), "mincore")
            return {"bytes": size, "pages": pages, "resident_pages": sum(n & 1 for n in vec)}


def process_alive(pidfd):
    ready = select.poll()
    ready.register(pidfd, select.POLLIN)
    return not ready.poll(0)


class CloseObservation:
    def __init__(self, scope, db, pidfd, cutoff_ms, out, sources):
        self.scope, self.db, self.pidfd = scope, db, pidfd
        self.cutoff, self.out = cutoff_ms / 1000, out
        # SCM_RIGHTS transfers an already-open, read-only procfs file from each
        # owned network namespace. All sampling is performed on the manager core.
        self.net_fds = []
        try:
            for sources_in_role in sources:
                fds = []
                self.net_fds.append(fds)
                for provider in sources_in_role:
                    fd = receive_namespace_fd(provider, b"Q")
                    fds.append(fd)
                    self.established(fd)  # Validate the procfs sequence file before sampling.
        except BaseException:
            self.close_net_fds()
            raise
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.watch, daemon=True)
        self.thread.start()

    def stop(self):
        self.stopped.set()
        self.thread.join()
        self.close_net_fds()

    def close_net_fds(self):
        for group in self.net_fds:
            for fd in group:
                os.close(fd)

    @staticmethod
    def established(fd):
        rows = [
            line.split()[1:] for line in os.pread(fd, 65536, 0).splitlines() if line.startswith(b"Tcp:")
        ]
        if len(rows) != 2 or b"CurrEstab" not in rows[0]:
            raise RuntimeError("private netns TCP counter unavailable")
        return int(rows[1][rows[0].index(b"CurrEstab")])

    def snapshot(self, phase):
        measurements = {}
        for name in ("memory.current", "memory.peak", "memory.events", "memory.stat"):
            try:
                raw = (self.scope / name).read_text()
            except FileNotFoundError:
                continue  # A scope can disappear when OOM kills its last native task.
            if name in ("memory.current", "memory.peak"):
                measurements[name] = int(raw)
                continue
            values = {key: int(v) for key, v in (line.split() for line in raw.splitlines())}
            measurements[name] = {
                k: v for k, v in values.items() if name == "memory.events" or k in MEMORY_FIELDS
            }
        if phase:
            measurements.update(mincore_phase=phase, mincore={})
            for f in (self.db, Path(str(self.db) + "-wal")):
                try:
                    measurements["mincore"][f.name] = resident_pages(f)
                except FileNotFoundError:
                    pass  # The WAL can be absent when no transaction needs it.
                except OSError as err:
                    measurements["mincore"][f.name] = {"error": str(err)}
        return measurements

    def watch(self):
        sampled = set()
        with (self.out / "closure.jsonl").open("w") as log:
            while True:
                wall = time.time()
                relative = wall - self.cutoff
                phase = None
                if "startup" not in sampled:
                    phase = "startup"
                elif relative >= -1 and "one_second_before_cutoff" not in sampled:
                    phase = "one_second_before_cutoff"
                if phase:
                    sampled.add(phase)
                row = {
                    "utc": wall,
                    "relative_to_cutoff_s": relative,
                    "native_alive": process_alive(self.pidfd),
                }
                row.update(self.snapshot(phase))
                row["native_established_plus_close_wait"] = [self.established(fd) for fd in self.net_fds[0]]
                row["client_established_plus_close_wait"] = [self.established(fd) for fd in self.net_fds[1]]
                print(json.dumps(row), file=log, flush=True)
                period = 0.1 if relative >= -4 else min(1, max(0.1, -4 - relative))
                if self.stopped.wait(period):
                    break


def check_health(client_provider, address):
    result = {"ip": address, "utc": time.time(), "ok": False}
    try:
        with socket.socket(fileno=tcp_socket(client_provider)) as client:
            client.settimeout(2)
            client.connect((address, 19432))
            client.sendall(b"GET /health HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n")
            with client.makefile("rb") as reader:
                status = reader.readline(513).decode("ascii").strip()
                headers = {}
                total = 0
                while True:
                    line = reader.readline(4097)
                    total += len(line)
                    if total > 4096:
                        raise ValueError("oversized health response headers")
                    if line == b"\r\n":
                        break
                    k, val = line.split(b":", 1)
                    headers[k.lower()] = val.strip()
                length = int(headers[b"content-length"])
                if not 0 < length <= 4096:
                    raise ValueError("invalid health response length")
                raw = reader.read(length)
                if len(raw) != length:
                    raise ValueError("incomplete health response")
                body = json.loads(raw)
                result.update(
                    status=status,
                    body=raw.decode(),
                    ok=status.startswith("HTTP/1.1 200 ")
                    and isinstance(body, dict)
                    and all(body.get(k) == "ok" for k in ("status", "db")),
                )
    except (OSError, KeyError, ValueError, UnicodeError, TypeError) as err:
        result["error"] = str(err)
    return result


def check_database(db, timeout=10):
    """Open the actual resulting native database in query-only mode, including its WAL."""
    started = time.monotonic()
    report = {"utc": time.time(), "inode": db.stat().st_ino}
    try:
        conn = sqlite3.connect(db.resolve().as_uri() + "?mode=ro", uri=True, timeout=timeout)
        try:
            conn.execute("PRAGMA query_only=ON")
            conn.execute("BEGIN")
            report["integrity_check"] = [row[0] for row in conn.execute("PRAGMA integrity_check")]
            report["foreign_key_check"] = conn.execute("PRAGMA foreign_key_check").fetchall()
            report["table_rows"] = {
                t: conn.execute("SELECT count(*) FROM " + t).fetchone()[0]
                for t in ("users", "posts", "likes")
            }
            conn.execute("ROLLBACK")
        finally:
            conn.close()
    except (sqlite3.Error, OSError) as err:
        report["error"] = str(err)
        if isinstance(err, sqlite3.Error):
            report["sqlite_errorcode"] = getattr(err, "sqlite_errorcode", None)
            report["sqlite_errorname"] = getattr(err, "sqlite_errorname", None)
    report["duration_s"] = time.monotonic() - started
    return report


def database_ok(check):
    return check.get("integrity_check") == ["ok"] and check.get("foreign_key_check") == []


def close_checks_pass(post, listeners, shards):
    before = post["live_db_verification"]
    after = post["poststop_db_verification"]
    # The stock unix-excl SQLite VFS can deny every independent reader for
    # the application's lifetime. Require integrity on the actual DB after
    # the native owner releases that lock, including any committed WAL.
    code = before.get("sqlite_errorcode")
    held_exclusive = code is not None and (code & 255) in (sqlite3.SQLITE_BUSY, sqlite3.SQLITE_LOCKED)
    inode = before.get("inode")
    return (
        listeners > 0
        and shards > 0
        and post["official_shard_exit"] == [0] * shards
        and post["native_by_link_established_plus_close_wait"] == [0] * listeners
        and post["client_by_shard_established_plus_close_wait"] == [0] * shards
        and post["native_alive_before_health"]
        and post["native_alive_after_health"]
        and post["native_alive_after_integrity"]
        and not post["native_alive_after_stop"]
        and len(post["health"]) == listeners
        and all(check["ok"] for check in post["health"])
        and isinstance(inode, int)
        and inode > 0
        and inode == after.get("inode")
        and (database_ok(before) or held_exclusive)
        and database_ok(after)
    )
