"""TokenVerifier for HS256 JWTs, checked by hand with hmac + hashlib (no JWT library)."""

import base64
import binascii
import hmac
import re

from adapters.json_codec import decode_json

_B64URL = re.compile(rb"[A-Za-z0-9_-]*")
# {"alg":"HS256","typ":"JWT"}, the header every load-test token carries: known good, so it isn't
# decoded (PR #84 does the same). Any other header is decoded and checked.
_STANDARD_HEADER = b"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"


def _b64url_decode(part: bytes) -> bytes:
    if not _B64URL.fullmatch(part):
        raise ValueError("not base64url")
    return base64.urlsafe_b64decode(part + b"=" * (-len(part) % 4))


class Hs256Verifier:
    __slots__ = ("_secret",)

    def __init__(self, secret: bytes):
        self._secret = secret

    def claims(self, token: str) -> dict | None:
        try:
            raw = token.encode("ascii")
            signing_input, _, signature = raw.rpartition(b".")
            head_b64, dot, payload_b64 = signing_input.partition(b".")
            if not dot or b"." in payload_b64:
                return None
            if head_b64 != _STANDARD_HEADER:
                head = decode_json(_b64url_decode(head_b64))
                if not isinstance(head, dict) or head.get("alg") != "HS256":
                    return None
            expected = base64.urlsafe_b64encode(hmac.digest(self._secret, signing_input, "sha256")).rstrip(b"=")
            if not hmac.compare_digest(expected, signature):
                return None
            payload = decode_json(_b64url_decode(payload_b64))
        except (ValueError, binascii.Error):  # UnicodeError is a ValueError
            return None
        return payload if isinstance(payload, dict) else None
