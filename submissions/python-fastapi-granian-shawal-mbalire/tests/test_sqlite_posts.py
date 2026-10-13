import asyncio
import json
import tempfile
import unittest
from pathlib import Path

from adapters.sqlite_posts import SqliteConfig, SqlitePostStore
from domain.models import LikeOutcome, Principal
from tests.fixtures import make_db


class SqlitePostStoreTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = make_db(Path(self.tmp.name) / "feed.db")
        self.store = SqlitePostStore(SqliteConfig(path=str(self.path)))
        await self.store.start()

    async def asyncTearDown(self):
        await self.store.close()
        self.tmp.cleanup()

    async def test_post_document_is_exact_compact_json(self):
        self.assertEqual(
            await self.store.post(1),
            b'{"post":{"id":1,"body":"first","created_at":"2025-01-01T00:00:00.000Z","author":"alice","like_count":2}}',
        )
        self.assertIsNone(await self.store.post(99))

    async def test_feed_newest_first_and_escaped(self):
        feed = await self.store.feed()
        self.assertEqual([p["id"] for p in json.loads(feed)["posts"]], [2, 3, 1])
        self.assertIn(b'"body":"say \\"hi\\""', feed)

    async def test_create_post_renders_author_from_token(self):
        document = json.loads(await self.store.create_post(Principal(2, "token-name"), "new"))["post"]
        self.assertEqual(list(document), ["id", "body", "created_at", "author", "like_count"])
        self.assertEqual((document["id"], document["author"], document["like_count"]), (4, "token-name", 0))
        self.assertRegex(document["created_at"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$")

    async def test_like_outcomes(self):
        self.assertIs(await self.store.like(1, 2), LikeOutcome.LIKED)
        self.assertIs(await self.store.like(1, 2), LikeOutcome.ALREADY_LIKED)
        self.assertIs(await self.store.like(1, 99), LikeOutcome.POST_MISSING)

    async def test_group_commit_batches_concurrent_writes(self):
        batches = []
        original = self.store._commit

        def counting_commit():
            batches.append(self.store._batch)
            original()

        self.store._commit = counting_commit
        outcomes = await asyncio.gather(*(self.store.like(user, 3) for user in (1, 2, 1)))
        self.assertEqual(outcomes, [LikeOutcome.LIKED, LikeOutcome.LIKED, LikeOutcome.ALREADY_LIKED])
        self.assertEqual(len(batches), 1)
        self.assertFalse(self.store._db.in_transaction)

    async def test_writes_are_durable_after_close(self):
        await self.store.like(2, 2)
        await self.store.close()
        reopened = SqlitePostStore(SqliteConfig(path=str(self.path)))
        self.store = reopened
        self.assertIn(b'"like_count":1', await reopened.post(2))

    async def test_ping(self):
        self.store.ping()


if __name__ == "__main__":
    unittest.main()
