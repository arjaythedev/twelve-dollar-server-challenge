"""Compare response encoding and the complete read-to-bytes path on the seed."""

import gc
import hashlib
import json
import platform
import random
import sqlite3
import statistics
import time
from collections.abc import Callable
from contextlib import closing
from functools import partial
from pathlib import Path
from typing import TypedDict, cast

import msgspec
import orjson

from api.models import Post

ROOT = Path(__file__).resolve().parent.parent
ROWS = """SELECT p.id, p.body, p.created_at, u.username AS author,
    (SELECT count(*) FROM likes WHERE post_id = p.id) AS like_count
    FROM posts p JOIN users u ON u.id = p.user_id"""
FEED = ROWS + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
POST = ROWS + " WHERE p.id = ?"
JSON_POST = "json_object('id',id,'body',body,'created_at',created_at,'author',author,'like_count',like_count)"
FIELDS = "id, body, created_at, author, like_count"

type Row = tuple[int, str, str, str, int]


class Measurement(TypedDict):
    case: str
    variant: str
    median_us: float
    min_us: float
    max_us: float


def stdlib(value: object) -> bytes:
    return json.dumps(value, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode(
        "ascii"
    )


def materialize(rows: list[Row]) -> list[Post]:
    return [
        Post(id=row[0], body=row[1], created_at=row[2], author=row[3], like_count=row[4])
        for row in rows
    ]


def python_read(
    db: sqlite3.Connection,
    sql: str,
    parameters: tuple[int, ...],
    key: str,
    encoder: Callable[[object], bytes],
) -> bytes:
    result = materialize(cast(list[Row], db.execute(sql, parameters).fetchall()))
    return encoder({key: result if key == "posts" else result[0]})


def query(db: sqlite3.Connection, statement: str, parameters: tuple[int, ...]) -> bytes:
    return cast(tuple[bytes], db.execute(statement, parameters).fetchone())[0]


def run(iterations: int, repeat: int, save: Path, database: Path = ROOT / "seed/feed.db") -> None:
    if iterations < 1 or repeat < 1:
        raise ValueError("iterations and repeat must be positive")
    measurements: list[Measurement] = []
    with closing(sqlite3.connect(database.resolve().as_uri() + "?mode=ro", uri=True)) as db:
        db.enable_load_extension(True)
        try:
            db.load_extension(str(Path(__file__).with_name("api") / "_json"))
        finally:
            db.enable_load_extension(False)
        for name, sql, parameters, key in (
            ("feed", FEED, (), "posts"),
            ("post", POST, (421337,), "post"),
        ):
            rows = cast(list[Row], db.execute(sql, parameters).fetchall())
            posts = materialize(rows)
            value = {key: posts if key == "posts" else posts[0]}
            expected = json.loads(stdlib(value))

            if key == "posts":
                builtin_sql = (
                    "SELECT CAST('{\"posts\":[' || coalesce(group_concat("
                    + JSON_POST
                    + " ORDER BY created_at DESC,id DESC), '') || ']}' AS BLOB) FROM ("
                    + sql
                    + ")"
                )
                native_sql = f"SELECT td_feed({FIELDS}) FROM ({sql})"
            else:
                builtin_sql = f"SELECT CAST(json_object('post',{JSON_POST}) AS BLOB) FROM ({sql})"
                native_sql = f"SELECT td_post({FIELDS}) FROM ({sql})"

            cases: dict[str, Callable[[], bytes]] = {
                "encode:stdlib": partial(stdlib, value),
                "encode:orjson": partial(orjson.dumps, value),
                "encode:msgspec": partial(msgspec.json.encode, value),
                "read:stdlib": partial(python_read, db, sql, parameters, key, stdlib),
                "read:orjson": partial(python_read, db, sql, parameters, key, orjson.dumps),
                "read:msgspec": partial(python_read, db, sql, parameters, key, msgspec.json.encode),
                "read:sqlite-json": partial(query, db, builtin_sql, parameters),
                "read:native": partial(query, db, native_sql, parameters),
            }
            timings: dict[str, list[float]] = {variant: [] for variant in cases}
            for function in cases.values():
                if json.loads(function()) != expected:
                    raise RuntimeError(f"{name}: output differs")
                for _ in range(100):
                    function()
            order = list(cases)
            rng = random.Random(0)
            for _ in range(repeat):
                rng.shuffle(order)
                for variant in order:
                    function = cases[variant]
                    was_enabled = gc.isenabled()
                    gc.disable()
                    try:
                        started = time.perf_counter_ns()
                        for _ in range(iterations):
                            function()
                        elapsed = time.perf_counter_ns() - started
                    finally:
                        if was_enabled:
                            gc.enable()
                    timings[variant].append(elapsed / iterations / 1000)
            for variant, samples in timings.items():
                row = Measurement(
                    case=name,
                    variant=variant,
                    median_us=statistics.median(samples),
                    min_us=min(samples),
                    max_us=max(samples),
                )
                measurements.append(row)
                print(f"{name:5} {variant:18} {row['median_us']:9.3f} us")
    save.parent.mkdir(parents=True, exist_ok=True)
    save.write_text(
        json.dumps(
            {
                "python": platform.python_version(),
                "platform": platform.platform(),
                "sqlite": sqlite3.sqlite_version,
                "native_sha256": hashlib.sha256(
                    (Path(__file__).with_name("native") / "json.c").read_bytes()
                ).hexdigest(),
                "orjson": orjson.__version__,
                "msgspec": msgspec.__version__,
                "iterations": iterations,
                "repeat": repeat,
                "rows": measurements,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
