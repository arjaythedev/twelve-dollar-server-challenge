"""Strict UTF-8 JSON with the API's required dictionary key order."""

import json
from typing import Never, cast

import orjson

from .models import JSONResponse

type JSONValue = bool | int | float | str | list[JSONValue] | dict[str, JSONValue] | None


def _reject_constant(value: str) -> Never:
    raise ValueError(f"invalid JSON constant: {value}")


def decode_json(data: bytes) -> JSONValue:
    # The standard decoder without object hooks produces exactly these JSON types.
    return cast(JSONValue, json.loads(data.decode("utf-8"), parse_constant=_reject_constant))


def encode_json(value: JSONResponse) -> bytes:
    try:
        return orjson.dumps(value)
    except orjson.JSONEncodeError:
        # JWT usernames may contain escaped lone surrogates, which orjson cannot encode.
        return json.dumps(value, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode(
            "ascii"
        )
