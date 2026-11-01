"""Keep profile categories exclusive, especially JSON work inside authentication."""

import tempfile
import unittest
from pathlib import Path

from request_profile import analyze, stage_for

REQUEST = "app.py:Application.__call__:111"


class ProfileTests(unittest.TestCase):
    def test_stage_ownership(self) -> None:
        for frames, expected in [
            (["events.py:Handle._run:94"], None),
            ([REQUEST, "codec.py:encode_json:20"], "response_json"),
            ([REQUEST, "codec.py:decode_json:15"], "request_json"),
            ([REQUEST, "auth.py:authenticate:40", "codec.py:decode_json:15"], "auth"),
            ([REQUEST, "db.py:Database.feed:51", "db.py:_post:22"], "row_mapping"),
            ([REQUEST, "db.py:Database.feed:50", "<native>"], "database"),
            ([REQUEST, "codec.py:encode_json:20", "<GC>"], "gc"),
            ([REQUEST, "app.py:Application.handle:72"], "request_other"),
        ]:
            with self.subTest(frames=frames):
                self.assertEqual(stage_for(frames), expected)

    def test_counts_partition_request_samples(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            (output / "cpu.collapsed").write_text(
                f"tid:1;<native>;{REQUEST};db.py:Database.feed:50 7\n"
                f"tid:1;<native>;{REQUEST};auth.py:authenticate:40;codec.py:decode_json:15 3\n"
                "tid:2;events.py:Handle._run:94 11\n",
                encoding="utf-8",
            )
            (output / "cpu-sampler.log").write_text(
                "Captured 1000 samples in 1.00 seconds\n"
                "Sample rate: 1000.00 samples/sec\nError rate: 0.30\n",
                encoding="utf-8",
            )
            result = analyze(output, "cpu")
        self.assertEqual(result["total_thread_samples"], 21)
        self.assertEqual(result["request_samples"], 10)
        self.assertEqual(result["outside_request_samples"], 11)
        self.assertEqual(sum(result["stages"].values()), 10)
        self.assertEqual(result["stages"]["auth"], 3)
        self.assertEqual(result["stages"]["request_json"], 0)
        self.assertEqual(result["database_methods"], {"db.py:Database.feed": 7})
        self.assertEqual(
            result["outside_top_frames"], [{"frame": "events.py:Handle._run:94", "samples": 11}]
        )


if __name__ == "__main__":
    unittest.main()
