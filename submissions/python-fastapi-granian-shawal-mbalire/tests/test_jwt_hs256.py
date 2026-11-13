import unittest

from adapters.jwt_hs256 import Hs256Verifier
from tests.fixtures import SECRET, sign

CLAIMS = {"sub": "1", "username": "alice", "exp": 2_000_000_000}


class Hs256VerifierTest(unittest.TestCase):
    def setUp(self):
        self.verifier = Hs256Verifier(SECRET)

    def test_valid_token(self):
        self.assertEqual(self.verifier.claims(sign(CLAIMS)), CLAIMS)

    def test_rejects(self):
        good = sign(CLAIMS)
        head, payload, _ = good.split(".")
        cases = {
            "wrong secret": sign(CLAIMS, secret=b"other"),
            "alg none": sign(CLAIMS, header={"alg": "none"}),
            "alg HS512": sign(CLAIMS, header={"alg": "HS512"}),
            "no signature": f"{head}.{payload}.",
            "two parts": f"{head}.{payload}",
            "four parts": good + ".x",
            "garbage": "abc",
            "non-ascii": good + "é",
            "bad base64": "!!!." + payload + ".x",
            "payload not an object": sign([1, 2]),  # type: ignore[arg-type]
        }
        for name, token in cases.items():
            with self.subTest(name):
                self.assertIsNone(self.verifier.claims(token))


if __name__ == "__main__":
    unittest.main()
