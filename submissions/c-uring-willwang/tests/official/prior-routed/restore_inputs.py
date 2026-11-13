"""Restore the historical benchmark inputs in a new directory using an ordinary challenge seed.

Run with a Python interpreter linked to the pinned Ubuntu SQLite 3.45.1 build.
The input database is opened immutable and all SQL writes are performed in memory.
"""

import argparse
import base64
import contextlib
import ctypes
import errno
import fcntl
import hashlib
import hmac
import json
import os
from pathlib import Path
import sqlite3
import tempfile
import time

SQLITE_SOURCE_ID = "2024-01-30 16:01:20 e876e51a0ed5c5b3126f52e532044363a014bc594cfefa87ffb5b82257ccalt1"
HEADER = bytes.fromhex(
    "53514c69746520666f726d617420330010000202004020200000000c0000eda6"
    "0000000000000000000000080000000400000000000000000000000100000000"
    "000000000000000000000000000000000000000000000000000000000000000c"
    "002e7689"
)
ISSUED = 1794464960
EXPECTED = {
    "feed.db": (
        249192448,
        "102193bcdb4ced52abb98f6bdfb19e06284273af30b74cccd009b257f09569cc",
    ),
    "tokens.json": (
        4859318,
        "7c4eb45a67156ab3827d6fadab40c884fcab77d4bc4051bffe9e537a16fffa50",
    ),
}


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def encode(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()


def base64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=")


def make_tokens(source, destination):
    users = json.loads(source.read_bytes())
    header = base64url(encode({"alg": "HS256", "typ": "JWT"}))
    key = hmac.new(b"twelve-dollar-challenge", digestmod="sha256")
    rows = []
    for index, user in enumerate(users, 1):
        if user["id"] != index:
            raise ValueError("Expected fresh challenge token records in user ID order")
        name = user["username"]
        payload = base64url(
            encode(
                {
                    "sub": str(index),
                    "username": name,
                    "iat": ISSUED,
                    "exp": ISSUED + 3650 * 86400,
                }
            )
        )
        message = header + b"." + payload
        signature = key.copy()
        signature.update(message)
        token = message + b"." + base64url(signature.digest())
        rows.append({"id": index, "username": name, "token": token.decode()})
    destination.write_bytes(encode(rows))


def make_database(source, destination):
    with contextlib.closing(
        sqlite3.connect(":memory:", isolation_level=None)
    ) as target:
        actual = target.execute("SELECT sqlite_source_id()").fetchone()[0]
        if actual != SQLITE_SOURCE_ID:
            raise ValueError(
                f"Expected Ubuntu SQLite 3.45.1 source ID {SQLITE_SOURCE_ID}, got {actual}"
            )
        target.execute("PRAGMA temp_store=MEMORY")
        target.execute("PRAGMA cache_size=-16000")
        with contextlib.closing(
            sqlite3.connect(source.as_uri() + "?mode=ro&immutable=1", uri=True)
        ) as original:
            original.backup(target)
        target.execute("VACUUM")
        image = memoryview(target.serialize())
        if len(HEADER) != 100 or len(image) != EXPECTED["feed.db"][0]:
            raise ValueError("Unexpected database header or geometry")
        with destination.open("xb") as output:
            output.write(HEADER)
            output.write(image[len(HEADER) :])


def publish_new(source, destination):
    # Linux RENAME_NOREPLACE avoids overwriting even an empty destination directory
    # created by another process during restoration.
    rename = ctypes.CDLL(None, use_errno=True).renameat2
    rename.argtypes = [
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_uint,
    ]
    rename.restype = ctypes.c_int
    if rename(-100, os.fsencode(source), -100, os.fsencode(destination), 1):
        error = ctypes.get_errno()
        if error == errno.EEXIST:
            raise FileExistsError(destination)
        raise OSError(error, os.strerror(error), destination)


def restore(seed, tokens, output, locks):
    if output.exists() or output.is_symlink():
        raise FileExistsError(output)
    if locks[0] == locks[1]:
        raise ValueError("Supply two distinct shared benchmark locks")
    if os.path.samefile(seed, tokens):
        raise ValueError(
            "Supply the challenge SQLite database and JSON token file separately"
        )
    with contextlib.ExitStack() as stack:
        lock_info = []
        for path in locks:
            handle = stack.enter_context(path.open("a"))
            fcntl.flock(handle, fcntl.LOCK_EX)
            info = os.fstat(handle.fileno())
            lock_info.append(
                {
                    "name": path.name,
                    "device": info.st_dev,
                    "inode": info.st_ino,
                    "acquired_unix": time.time(),
                }
            )
        if output.exists() or output.is_symlink():
            raise FileExistsError(output)
        with tempfile.TemporaryDirectory(
            dir=output.parent, prefix=".priorroute-restore-"
        ) as td:
            prepared = Path(td) / "verified"
            prepared.mkdir()
            make_database(seed, prepared / "feed.db")
            make_tokens(tokens, prepared / "tokens.json")
            actual = {}
            for name, (length, checksum) in EXPECTED.items():
                path = prepared / name
                measured = (path.stat().st_size, digest(path))
                if measured != (length, checksum):
                    raise ValueError(
                        f"{name} did not match the archived size and SHA-256: {measured}"
                    )
                path.chmod(0o444)
                actual[name] = {"bytes": length, "sha256": checksum}
            manifest = {
                "files": actual,
                "sqlite_source_id": SQLITE_SOURCE_ID,
                "sqlite_header": HEADER.hex(),
                "token_issued_unix": ISSUED,
                "source": {
                    "database_sha256": digest(seed),
                    "tokens_sha256": digest(tokens),
                },
                "volume_device": output.parent.stat().st_dev,
                "locks": lock_info,
            }
            (prepared / "manifest.json").write_text(
                json.dumps(manifest, indent=2) + "\n"
            )
            (prepared / "manifest.json").chmod(0o444)
            publish_new(prepared, output)
    return {"directory": str(output), "files": actual}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--seed", type=Path, required=True, help="fresh challenge feed.db"
    )
    parser.add_argument(
        "--tokens", type=Path, required=True, help="fresh challenge tokens.json"
    )
    parser.add_argument(
        "--output", type=Path, required=True, help="new directory on an existing volume"
    )
    parser.add_argument("--component-lock", type=Path, required=True)
    parser.add_argument("--serial-lock", type=Path, required=True)
    args = parser.parse_args()
    destination = args.output.absolute()
    print(
        json.dumps(
            restore(
                args.seed.resolve(strict=True),
                args.tokens.resolve(strict=True),
                destination,
                [args.component_lock.resolve(), args.serial_lock.resolve()],
            ),
            indent=2,
        )
    )
