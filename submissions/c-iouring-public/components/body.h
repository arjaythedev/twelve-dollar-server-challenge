/* db/body.h (sha256 c004d2c6b29d), developed separately for this entry. */
#ifndef ENG_BODY_H
#define ENG_BODY_H
/*
 * Strict request-body parser for POST /posts.
 *
 * Reference behaviour (Node): bytes -> new TextDecoder('utf-8',{fatal:true, ignoreBOM:true}) -> JSON.parse
 *   -> obj = (non-null, non-array object) ? obj.body : undefined -> typeof === 'string' -> .trim() -> length check
 *   -> Buffer.from(t,'utf8'). Verified differentially against node v24 (oracle.mjs, body_corpus/, test_body.c).
 *
 * Decisions for the ambiguous cases:
 *  - Empty input / only whitespace           -> MALFORMED.
 *  - JSON whitespace is exactly SP, HT, LF, CR (anything else between tokens, e.g. U+00A0 or VT, -> MALFORMED).
 *  - Trailing garbage after the single top-level value (incl. a second value, a NUL byte) -> MALFORMED.
 *  - Leading UTF-8 BOM (EF BB BF)             -> MALFORMED (JSON.parse rejects U+FEFF; we behave as if the decoder
 *                                               did not strip it).
 *  - Invalid UTF-8 anywhere (overlongs, C0/C1, F5..FF, stray continuation bytes, truncated sequences, UTF-8-encoded
 *    surrogates ED A0..BF, > U+10FFFF)      -> MALFORMED. Raw bytes < 0x20 inside strings -> MALFORMED.
 *  - Grammar is RFC 8259 exactly: numbers -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?, literals true/false/null,
 *    escapes \" \\ \/ \b \f \n \r \t \uXXXX (hex any case). No comments, no trailing commas, no single quotes.
 *    Values are validated even when ignored (nested, non-body keys, earlier duplicate keys).
 *  - Malformed always wins over the other errors: the whole document is validated before any other verdict.
 *  - Top-level value not an object (array, string, number, true/false/null) -> REQUIRED (in JS `null.body` would
 *    throw; we report "body is required" for all of them).
 *  - Keys are compared after unescaping ("body" is body). Only the top-level object's keys count; keys of
 *    nested objects are ignored. Duplicate keys: the LAST top-level "body" wins (JSON.parse), so
 *    {"body":"x","body":1} -> REQUIRED and {"body":1,"body":"x"} -> OK "x".
 *  - `body` missing or not a string -> REQUIRED.
 *  - \uD8xx\uDCxx pairs combine into one astral code point. A lone high or lone low surrogate escape is emitted
 *    as U+FFFD (EF BF BD), as Node does when encoding the JS string to UTF-8. It counts as 1 code point and
 *    1 UTF-16 unit (it is not whitespace, so trimming is unaffected by the replacement).
 *  - Trim = JS String.prototype.trim: U+0009-000D, U+0020, U+00A0, U+1680, U+2000-200A, U+2028, U+2029, U+202F,
 *    U+205F, U+3000, U+FEFF from both ends. U+0085, U+180E, U+200B and NUL are NOT trimmed. Empty -> REQUIRED.
 *  - Length limit 500 on the trimmed string, checked after REQUIRED. len_utf16=0 counts Unicode code points
 *    (default; equals SQLite length() and the schema CHECK), len_utf16=1 counts UTF-16 code units (JS .length,
 *    astral chars count 2).
 *  - \u0000 is allowed and becomes a NUL byte in the output (*outn includes it; a later DB CHECK may reject it).
 *  - Nesting: iterative parser with a 32768-level bit stack, no recursion, no allocation. Deeper nesting returns
 *    MALFORMED; that can only happen for inputs > 32 KB (Node would accept them), well above our 16 KB limit.
 *  - cap < n is a caller bug: returns -1 and writes nothing. cap >= n always suffices (decoded output is never
 *    longer than its JSON source: a 6-byte lone-surrogate escape becomes 3 bytes, a 12-byte pair 4 bytes).
 */
#include <stddef.h>

enum { ENG_BODY_OK = 0, ENG_BODY_MALFORMED = 1 /* 400 {"error":"malformed JSON body"} */,
       ENG_BODY_REQUIRED = 2 /* 400 {"error":"body is required"} */,
       ENG_BODY_TOO_LONG = 3 /* 400 {"error":"body must be at most 500 characters"} */ };

/* Parses the raw request body (bytes as received). On OK writes the trimmed `body` string as UTF-8 to out
 * (not NUL-terminated) and sets *outn. len_utf16: 0 = count Unicode code points, 1 = count UTF-16 code units.
 * out is used as scratch on every path; *outn is only meaningful on OK. */
int eng_parse_post_body(const char *json, size_t n, char *out, size_t cap, size_t *outn, int len_utf16);

#endif
