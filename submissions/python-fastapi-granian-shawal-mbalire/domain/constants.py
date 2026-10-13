"""Fixed business rules from SPEC.md."""

MAX_BODY_CHARS = 500
MAX_ROW_ID = 2**63 - 1  # SQLite's INTEGER range; larger ids cannot exist
BEARER_PREFIX = "Bearer "

# JavaScript's String.prototype.trim() set (WhiteSpace + LineTerminator), so post bodies are
# trimmed exactly like the reference implementations.
JS_WHITESPACE = (
    "\t\n\v\f\r          "
    "        　﻿"
)
