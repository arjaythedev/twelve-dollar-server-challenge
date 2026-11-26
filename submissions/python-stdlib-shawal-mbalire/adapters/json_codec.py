"""Strict JSON decoding with the standard library's `json`, for request bodies and JWT parts.

`json.loads` is laxer than RFC 8259 (and than orjson, which the FastAPI entry used): it accepts
NaN and Infinity, guesses UTF-16/32 for bytes, and lets escapes decode to lone surrogates. Here each
of those is a ValueError, so the workflow answers "malformed JSON body" (or the token is invalid)
instead of failing later with a 500 when SQLite can't encode the text.
"""

import json


def _reject_constant(name: str) -> object:
    raise ValueError("not JSON: " + name)


_decode = json.JSONDecoder(parse_constant=_reject_constant).decode


def decode_json(raw: bytes) -> object:
    """The value `raw` holds. Raises ValueError (UnicodeError included) unless it is strict JSON in UTF-8."""
    value = _decode(raw.decode())
    if b"\\u" in raw:  # rare: an escape may have produced a lone surrogate
        json.dumps(value, ensure_ascii=False).encode()
    return value
