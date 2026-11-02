import unittest

from domain.errors import ErrorCode
from domain.models import Err, Ok, Principal
from domain.rules import bearer_token, clean_body, parse_post_id, principal_from_claims

NOW = 1_000_000.0


def code(result):
    return result.error.code if isinstance(result, Err) else None


class ParsePostId(unittest.TestCase):
    def test_positive_decimal(self):
        self.assertEqual(parse_post_id("42"), 42)

    def test_rejects(self):
        for raw in ("0", "-1", "abc", "1.5", "", "+1", "١٢", " 1"):
            with self.subTest(raw=raw):
                self.assertIsNone(parse_post_id(raw))


class BearerToken(unittest.TestCase):
    def test_extracts(self):
        self.assertEqual(bearer_token("Bearer abc"), "abc")

    def test_missing_or_other_scheme(self):
        for header in (None, "Basic abc", "bearer abc", "Bearerabc"):
            with self.subTest(header=header):
                self.assertIsNone(bearer_token(header))


class PrincipalFromClaims(unittest.TestCase):
    def test_valid(self):
        result = principal_from_claims({"sub": "7", "username": "u", "exp": NOW + 1}, NOW)
        self.assertEqual(result, Ok(Principal(7, "u")))

    def test_expired_or_not_yet_valid(self):
        for claims in ({"exp": NOW}, {"exp": "soon"}, {"exp": True}, {"nbf": NOW + 1}):
            with self.subTest(claims=claims):
                result = principal_from_claims({"sub": "1", "username": "u", **claims}, NOW)
                self.assertEqual(code(result), ErrorCode.INVALID_TOKEN)

    def test_bad_payload(self):
        for claims in (
            {"sub": "abc", "username": "u"},
            {"sub": 1, "username": "u"},
            {"sub": "0", "username": "u"},
            {"sub": str(2**63), "username": "u"},
            {"sub": "1"},
            {"sub": "1", "username": 5},
        ):
            with self.subTest(claims=claims):
                self.assertEqual(code(principal_from_claims(claims, NOW)), ErrorCode.INVALID_TOKEN_PAYLOAD)


class CleanBody(unittest.TestCase):
    def test_trims_js_whitespace(self):
        self.assertEqual(clean_body({"body": "  hi ﻿\n"}), Ok("hi"))

    def test_required(self):
        for payload in ({}, {"body": ""}, {"body": "   "}, {"body": 123}, [], "body", None):
            with self.subTest(payload=payload):
                self.assertEqual(code(clean_body(payload)), ErrorCode.BODY_REQUIRED)

    def test_length_counts_characters_after_trim(self):
        self.assertEqual(clean_body({"body": "  " + "é" * 500 + "  "}), Ok("é" * 500))
        self.assertEqual(code(clean_body({"body": "a" * 501})), ErrorCode.BODY_TOO_LONG)


if __name__ == "__main__":
    unittest.main()
