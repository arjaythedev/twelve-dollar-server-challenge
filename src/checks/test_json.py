"""Native JSON must preserve strings, integer widths, ordering and empty results."""

import json
import random
import sqlite3
import unittest
from pathlib import Path
from typing import cast


class NativeJSONTests(unittest.TestCase):
    def setUp(self) -> None:
        self.connection = sqlite3.connect(":memory:")
        self.addCleanup(self.connection.close)
        self.connection.enable_load_extension(True)
        try:
            self.connection.load_extension(str(Path(__file__).resolve().parents[1] / "api/_json"))
        finally:
            self.connection.enable_load_extension(False)

    def encode(self, body: str, author: str = "author", post_id: int = 1) -> bytes:
        return cast(
            tuple[bytes],
            self.connection.execute(
                "SELECT td_post(?, ?, '2026-01-01T00:00:00.000Z', ?, ?)",
                (post_id, body, author, 2**63 - 1),
            ).fetchone(),
        )[0]

    def test_strings_and_block_boundaries(self) -> None:
        for text in (
            "",
            '"\\/\b\f\n\r\t\0',
            "".join(chr(i) for i in range(32)),
            "café <>&/ \N{SNOWMAN} \N{GRINNING FACE}",
            "\N{GRINNING FACE}" * 500,
            *["a" * n + '"' + "b" * (35 - n) for n in range(36)],
            *["a" * n for n in range(40)],
        ):
            with self.subTest(text=text):
                raw = self.encode(text, text)
                expected = {
                    "post": {
                        "id": 1,
                        "body": text,
                        "created_at": "2026-01-01T00:00:00.000Z",
                        "author": text,
                        "like_count": 2**63 - 1,
                    }
                }
                self.assertEqual(
                    raw, json.dumps(expected, ensure_ascii=False, separators=(",", ":")).encode()
                )

    def test_signed_integer_boundaries(self) -> None:
        for post_id in (0, 1, -1, 2**63 - 1, -(2**63)):
            with self.subTest(post_id=post_id):
                self.assertEqual(
                    json.loads(self.encode("body", post_id=post_id))["post"]["id"], post_id
                )

    def test_random_strings_against_stdlib(self) -> None:
        rng = random.Random(42)
        alphabet = [chr(i) for i in range(128)] + ["é", "\u2028", "\N{GRINNING FACE}"]
        for _ in range(1000):
            text = "".join(rng.choices(alphabet, k=rng.randrange(501)))
            raw = self.encode(text, text)
            self.assertEqual(json.loads(raw)["post"]["body"], text)
            self.assertEqual(json.loads(raw)["post"]["author"], text)

    def test_empty_and_ordered_feed_with_buffer_growth(self) -> None:
        self.connection.execute("CREATE TABLE input(id INTEGER, body TEXT)")
        sql = "SELECT td_feed(id, body, 'time', 'author', 0) FROM input"
        self.assertEqual(self.connection.execute(sql).fetchone(), (b'{"posts":[]}',))
        self.connection.executemany(
            "INSERT INTO input VALUES(?, ?)", [(i, "\N{GRINNING FACE}" * 500) for i in range(20)]
        )
        first = cast(tuple[bytes], self.connection.execute(sql).fetchone())[0]
        self.assertEqual([p["id"] for p in json.loads(first)["posts"]], list(reversed(range(20))))
        self.connection.execute("UPDATE input SET body='fresh' WHERE id=19")
        second = cast(tuple[bytes], self.connection.execute(sql).fetchone())[0]
        self.assertEqual(json.loads(second)["posts"][0]["body"], "fresh")
        self.assertEqual(json.loads(first)["posts"][0]["body"], "\N{GRINNING FACE}" * 500)

    def test_feed_sorts_raw_timestamp_bytes_before_id(self) -> None:
        rows = [(0, ""), (1, "z"), (4, "a"), (3, "z"), (2, 'a"'), (5, "é"), (6, "a\0")]
        self.connection.execute("CREATE TABLE input(id INTEGER, timestamp TEXT)")
        self.connection.executemany("INSERT INTO input VALUES(?, ?)", rows)
        raw = cast(
            tuple[bytes],
            self.connection.execute(
                "SELECT td_feed(id, 'body', timestamp, 'author', 0) FROM input"
            ).fetchone(),
        )[0]
        expected = self.connection.execute(
            "SELECT id FROM input ORDER BY timestamp DESC,id DESC"
        ).fetchall()
        self.assertEqual([p["id"] for p in json.loads(raw)["posts"]], [r[0] for r in expected])

    def test_feed_rejects_more_than_twenty_rows(self) -> None:
        with self.assertRaises(sqlite3.DataError):
            self.connection.execute(
                "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<21) "
                "SELECT td_feed(x, 'body', 'time', 'author', 0) FROM n"
            ).fetchone()

    def test_invalid_text_raises_and_next_query_still_works(self) -> None:
        with self.assertRaises(sqlite3.OperationalError):
            self.connection.execute("SELECT td_post(1, NULL, 'time', 'author', 0)").fetchone()
        self.assertEqual(json.loads(self.encode("valid"))["post"]["body"], "valid")
