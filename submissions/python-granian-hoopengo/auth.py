import base64
import hashlib
import hmac
import json
import os
import time
from typing import Optional, Tuple

# Python 3.15: frozendict for constants (faster than a regular dict)
try:
    from builtins import frozendict
except ImportError:
    frozendict = dict  # fallback

JWT_SECRET = os.environ.get("JWT_SECRET", "twelve-dollar-challenge").encode()

# Pre-computed headers (PR #13: minimise overhead)
_AUTH_HEADER = b"authorization"
_BEARER_PREFIX = b"Bearer "


def check_auth(scope: dict) -> Tuple[Optional[int], Optional[str], Optional[str]]:
    """
    Returns (user_id, username, error_message).
    If there is no error, error_message is None.
    """
    headers = scope.get("headers", [])
    auth_header = None
    for name, value in headers:
        if name.lower() == _AUTH_HEADER:
            auth_header = value
            break

    if not auth_header:
        return None, None, "missing bearer token"
    if not auth_header.startswith(_BEARER_PREFIX):
        return None, None, "missing bearer token"

    token = auth_header[7:].decode()
    try:
        parts = token.split(".")
        if len(parts) != 3:
            return None, None, "invalid or expired token"

        header_b64, payload_b64, signature_b64 = parts

        header = json.loads(base64.urlsafe_b64decode(header_b64 + "=="))
        if header.get("alg") != "HS256":
            return None, None, "invalid or expired token"

        payload = json.loads(base64.urlsafe_b64decode(payload_b64 + "=="))

        signing_input = f"{header_b64}.{payload_b64}".encode()
        expected_sig = hmac.new(JWT_SECRET, signing_input, hashlib.sha256).digest()
        actual_sig = base64.urlsafe_b64decode(signature_b64 + "==")
        if not hmac.compare_digest(expected_sig, actual_sig):
            return None, None, "invalid or expired token"

        if "exp" in payload and payload["exp"] < time.time():
            return None, None, "invalid or expired token"

        sub = payload.get("sub")
        if not isinstance(sub, str) or not sub.isdigit() or int(sub) <= 0:
            return None, None, "invalid token payload"

        username = payload.get("username")
        if not isinstance(username, str):
            return None, None, "invalid token payload"

        return int(sub), username, None

    except Exception:
        return None, None, "invalid or expired token"
