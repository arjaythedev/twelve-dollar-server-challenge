"""Check the native reader at its transaction, schema and output-buffer boundaries."""

import ctypes
import json
import sqlite3
import tempfile
import unittest
from pathlib import Path


SUBMISSION = Path(__file__).resolve().parents[1]
SCHEMA = (SUBMISSION.parents[1] / "schema.sql").read_text()
FEED = """
    SELECT p.id, p.body, p.created_at, u.username AS author,
           (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
    FROM posts p JOIN users u ON u.id = p.user_id
    ORDER BY p.created_at DESC, p.id DESC LIMIT 20
"""


class RankSnapshot(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = ctypes.CDLL(str(SUBMISSION / "out" / "libsqlite3td.so"))
        ptr, text, integer = ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int
        signatures = {
            "sqlite3_initialize": (integer, []),
            "sqlite3_open": (integer, [text, ctypes.POINTER(ptr)]),
            "sqlite3_close": (integer, [ptr]),
            "sqlite3_errmsg": (text, [ptr]),
            "sqlite3_exec": (integer, [ptr, text, ptr, ptr, ptr]),
            "td_rank_open": (ptr, [ptr]),
            "td_rank_close": (None, [ptr]),
            "td_rank_render": (integer, [ptr, integer, ctypes.c_int64, ptr, integer]),
        }
        for name, (result, arguments) in signatures.items():
            method = getattr(cls.lib, name)
            method.restype, method.argtypes = result, arguments
        if cls.lib.sqlite3_initialize() != 0:
            raise RuntimeError("SQLite initialization failed")

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "feed.db"
        self.writer = sqlite3.connect(path, isolation_level=None)
        self.addCleanup(self.writer.close)
        self.writer.row_factory = sqlite3.Row
        self.writer.executescript(SCHEMA)
        self.writer.execute("PRAGMA journal_mode=WAL")
        self.writer.execute("INSERT INTO users (id, username) VALUES (1, 'Zoë')")
        self.writer.executemany(
            "INSERT INTO posts (id,user_id,body,created_at) VALUES (?,1,?,?)",
            [(i, f'post {i} "line"\n', f"2026-11-{i:02}T00:00:00.000Z") for i in range(1, 28)],
        )
        self.db = ctypes.c_void_p()
        self.assertEqual(self.lib.sqlite3_open(bytes(path), ctypes.byref(self.db)), 0)
        self.addCleanup(self.lib.sqlite3_close, self.db)
        self.sql("SELECT count(*) FROM sqlite_schema")
        self.rank = self.lib.td_rank_open(self.db)
        self.assertTrue(self.rank)
        self.addCleanup(self.lib.td_rank_close, self.rank)

    def sql(self, sql):
        code = self.lib.sqlite3_exec(self.db, sql.encode(), None, None, None)
        self.assertEqual(code, 0, self.lib.sqlite3_errmsg(self.db))

    def expected_feed(self):
        rows = [dict(row) for row in self.writer.execute(FEED)]
        return json.dumps({"posts": rows}, ensure_ascii=False, separators=(",", ":")).encode()

    def render(self, *, post=None, capacity=8192):
        guard = b"\xa5" * 32
        storage = ctypes.create_string_buffer(guard + b"\xa5" * capacity + guard)
        length = self.lib.td_rank_render(
            self.rank, int(post is None), post or 0, ctypes.byref(storage, len(guard)), capacity
        )
        self.assertEqual(storage.raw[:len(guard)], guard)
        self.assertEqual(storage.raw[len(guard) + capacity:-1], guard)
        self.assertGreaterEqual(length, -1)
        self.assertLessEqual(length, capacity)
        if length >= 0:
            return storage.raw[len(guard):len(guard) + length]

    def test_pinned_snapshot_and_rollback(self):
        self.assertIsNone(self.render())
        self.sql("BEGIN")
        first = self.expected_feed()
        self.assertEqual(self.render(), first)
        self.writer.execute("INSERT INTO posts VALUES (100,1,'external commit','2027-01-01T00:00:00.000Z')")
        second = self.expected_feed()
        self.assertNotEqual(second, first)
        self.assertEqual(self.render(), first)
        self.sql("COMMIT; BEGIN")
        self.assertEqual(self.render(), second)
        self.sql("INSERT INTO likes (user_id,post_id) VALUES (1,100)")
        self.assertEqual(json.loads(self.render(post=100))["post"]["like_count"], 1)
        self.sql("ROLLBACK; BEGIN")
        self.assertEqual(json.loads(self.render(post=100))["post"]["like_count"], 0)
        self.assertEqual(self.render(), second)

    def test_output_bounds_and_feed_proof(self):
        self.sql("BEGIN")
        wanted = self.expected_feed()
        for size in (0, 1, 23, 512, len(wanted) - 1):
            with self.subTest(capacity=size):
                self.assertIsNone(self.render(capacity=size))
        # A row may reserve its worst-case escaped length before copying it.
        exact = self.render(capacity=len(wanted))
        self.assertIn(exact, (None, wanted))
        self.assertEqual(self.render(), wanted)
        self.sql("COMMIT")
        self.writer.execute("UPDATE posts SET created_at='2030-01-01T00:00:00.000Z' WHERE id=1")
        self.assertEqual(json.loads(self.expected_feed())["posts"][0]["id"], 1)
        self.sql("BEGIN")
        self.assertIsNone(self.render())

    def test_external_schema_change(self):
        self.writer.executescript("DROP INDEX posts_created_at_id_idx; CREATE INDEX posts_created_at_id_idx ON posts(id)")
        self.sql("BEGIN")
        self.assertIsNone(self.render())

    def test_temp_shadow(self):
        self.sql("CREATE TEMP TABLE posts AS SELECT * FROM main.posts LIMIT 1; BEGIN")
        self.assertIsNone(self.render())
        self.assertFalse(self.lib.td_rank_open(self.db))


if __name__ == "__main__":
    unittest.main()
