import json
import unittest

from domain.errors import ErrorCode
from domain.models import Err, LikeReceipt, Ok, Principal
from domain.workflows import FeedApi
from tests.fixtures import FakeStore, FakeVerifier, FixedClock, bearer

GOOD = bearer({"sub": "1", "username": "alice"})


def code(result):
    return result.error.code if isinstance(result, Err) else None


class Workflows(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.store = FakeStore({5: b'{"post":5}'})
        self.clock = FixedClock(monotonic=10.0)
        self.api = FeedApi(self.store, FakeVerifier(), self.clock, decode_json=json.loads)

    def test_health_reports_uptime(self):
        self.clock.mono = 42.9
        self.assertEqual(self.api.health(), Ok(32))

    def test_health_degraded(self):
        self.store.up = False
        result = self.api.health()
        self.assertEqual((code(result), result.error.detail), (ErrorCode.STORE_UNREACHABLE, "disk on fire"))

    async def test_post(self):
        self.assertEqual(await self.api.post("5"), Ok(b'{"post":5}'))
        self.assertEqual(code(await self.api.post("6")), ErrorCode.POST_NOT_FOUND)
        self.assertEqual(code(await self.api.post(str(2**63))), ErrorCode.POST_NOT_FOUND)
        self.assertEqual(code(await self.api.post("x")), ErrorCode.INVALID_POST_ID)

    async def test_create_checks_auth_then_json_then_body(self):
        self.assertEqual(code(await self.api.create_post(None, b"{bad")), ErrorCode.MISSING_TOKEN)
        self.assertEqual(code(await self.api.create_post("Bearer nope", b"{bad")), ErrorCode.INVALID_TOKEN)
        self.assertEqual(code(await self.api.create_post(GOOD, b"{bad")), ErrorCode.MALFORMED_JSON)
        self.assertEqual(code(await self.api.create_post(GOOD, b"{}")), ErrorCode.BODY_REQUIRED)
        self.assertEqual(self.store.created, [])

    async def test_create(self):
        result = await self.api.create_post(GOOD, b'{"body":"  hello "}')
        self.assertEqual(result, Ok(b'{"post":"new"}'))
        self.assertEqual(self.store.created, [(Principal(1, "alice"), "hello")])

    async def test_like_checks_auth_before_id(self):
        self.assertEqual(code(await self.api.like(None, "abc")), ErrorCode.MISSING_TOKEN)
        self.assertEqual(code(await self.api.like(GOOD, "abc")), ErrorCode.INVALID_POST_ID)

    async def test_like(self):
        self.assertEqual(await self.api.like(GOOD, "5"), Ok(LikeReceipt(5, already_liked=False)))
        self.assertEqual(await self.api.like(GOOD, "5"), Ok(LikeReceipt(5, already_liked=True)))
        self.assertEqual(code(await self.api.like(GOOD, "6")), ErrorCode.POST_NOT_FOUND)
        self.assertEqual(code(await self.api.like(GOOD, str(2**63))), ErrorCode.POST_NOT_FOUND)


if __name__ == "__main__":
    unittest.main()
