"""Verify every HS256 token, including its required expiration claim."""

import base64
import binascii
import hmac
import math
import re
import time
from dataclasses import dataclass

from .codec import decode_json


@dataclass(frozen=True, slots=True)
class Identity:
    user_id: int
    username: str


class AuthenticationError(Exception):
    pass


def _decode_segment(segment: bytes) -> bytes:
    if re.fullmatch(rb"[A-Za-z0-9_-]+", segment) is None:
        raise ValueError("invalid base64url")
    return base64.b64decode(segment + b"=" * (-len(segment) % 4), altchars=b"-_", validate=True)


def authenticate(header: str, secret: bytes) -> Identity:
    if not header.startswith("Bearer "):
        raise AuthenticationError("missing bearer token")
    try:
        head, payload, signature = header[7:].encode("ascii").split(b".")
        metadata = decode_json(_decode_segment(head))
        if not isinstance(metadata, dict) or metadata.get("alg") != "HS256":
            raise ValueError("invalid algorithm")
        expected = hmac.digest(secret, head + b"." + payload, "sha256")
        if not hmac.compare_digest(expected, _decode_segment(signature)):
            raise ValueError("invalid signature")
        claims = decode_json(_decode_segment(payload))
        if not isinstance(claims, dict):
            raise ValueError("invalid claims")
        expires = claims.get("exp")
        if (
            isinstance(expires, bool)
            or not isinstance(expires, (int, float))
            or not math.isfinite(expires)
            or expires <= time.time()
        ):
            raise ValueError("invalid expiration")
    except ValueError, TypeError, UnicodeError, binascii.Error, OverflowError:
        raise AuthenticationError("invalid or expired token") from None

    subject = claims.get("sub")
    username = claims.get("username")
    if (
        not isinstance(subject, str)
        or re.fullmatch(r"[0-9]+", subject) is None
        or not isinstance(username, str)
    ):
        raise AuthenticationError("invalid token payload")
    try:
        user_id = int(subject)
    except ValueError:
        raise AuthenticationError("invalid token payload") from None
    if user_id <= 0:
        raise AuthenticationError("invalid token payload")
    return Identity(user_id, username)
