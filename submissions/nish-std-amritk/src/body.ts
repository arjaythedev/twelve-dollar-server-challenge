// `POST /posts`'s request body, `{"body":"..."}`, read as SPEC.md asks: any
// JSON text that does not parse is "malformed JSON body"; one that parses but
// is not an object with a string `body` (the last one, as in JSON.parse) is
// "body is required"; the string is trimmed with JavaScript's `trim()`
// whitespace set and counted in code points, the unit of the schema's
// `CHECK (length(body) BETWEEN 1 AND 500)`.
import {
  JSON_TOK_KEY,
  JSON_TOK_OBJECT_BEGIN,
  JSON_TOK_OBJECT_END,
  JSON_TOK_STRING,
  JsonLimits,
  JsonTokenizer,
} from "nish/json-tokenizer"

export const BODY_OK: i32 = 0
export const BODY_MALFORMED: i32 = 1
export const BODY_REQUIRED: i32 = 2
export const BODY_TOO_LONG: i32 = 3

/** The most characters a post may have. */
const BODY_MAX_CHARS: i32 = 500

/**
 * The length of the whitespace `trim()` removes that starts at `b[k]`
 * (before `end`): 1 to 3 bytes, or 0 when `b[k]` starts none. The set is
 * ECMAScript's WhiteSpace and LineTerminator: TAB to CR, SPACE, U+00A0,
 * U+1680, U+2000 to U+200A, U+2028, U+2029, U+202F, U+205F, U+3000, U+FEFF.
 */
const spaceAt = (b: u8[], k: i32, end: i32): i32 => {
  const c: i32 = toI32(b[k])
  if (c === 32 || (c >= 9 && c <= 13)) {
    return 1
  }
  if (c < 0xc2) {
    return 0
  }
  const c1: i32 = k + 1 < end ? toI32(b[k + 1]) : 0
  if (c === 0xc2) {
    return c1 === 0xa0 ? 2 : 0
  }
  if (k + 2 >= end) {
    return 0
  }
  const c2: i32 = toI32(b[k + 2])
  if (c === 0xe1) {
    return c1 === 0x9a && c2 === 0x80 ? 3 : 0
  }
  if (c === 0xe2) {
    if (c1 === 0x80) {
      return c2 <= 0x8a || c2 === 0xa8 || c2 === 0xa9 || c2 === 0xaf ? 3 : 0
    }
    return c1 === 0x81 && c2 === 0x9f ? 3 : 0
  }
  if (c === 0xe3) {
    return c1 === 0x80 && c2 === 0x80 ? 3 : 0
  }
  if (c === 0xef) {
    return c1 === 0xbb && c2 === 0xbf ? 3 : 0
  }
  return 0
}

/** The length of the whitespace `trim()` removes that ends just before `b[end]` (after `start`). */
const spaceBefore = (b: u8[], start: i32, end: i32): i32 => {
  const c: i32 = toI32(b[end - 1])
  if (c === 32 || (c >= 9 && c <= 13)) {
    return 1
  }
  if (c < 0x80) {
    return 0
  }
  if (end - 2 >= start && spaceAt(b, end - 2, end) === 2) {
    return 2
  }
  if (end - 3 >= start && spaceAt(b, end - 3, end) === 3) {
    return 3
  }
  return 0
}

/** Reads one request body at a time into `text`, trimmed. */
export class BodyReader {
  tokenizer: JsonTokenizer
  /** The decoded `body` string; the trimmed post is `text[start, end)`. */
  text: u8[]
  start: i32 = 0
  end: i32 = 0

  constructor() {
    const limits = new JsonLimits()
    limits.maxDepth = 64
    this.tokenizer = new JsonTokenizer(new Array<u8>(0), 0, 0, limits)
    this.text = new Array<u8>(4096)
  }

  /** Reads `src[off, off + len)`; answers a `BODY_` code, with the post in `text[start, end)` on `BODY_OK`. */
  read(src: u8[], off: i32, len: i32): i32 {
    const t: JsonTokenizer = this.tokenizer
    t.reset(src, off, len)
    let k: i32 = t.next()
    if (k < 0) {
      return BODY_MALFORMED
    }
    let found: boolean = false
    let n: i32 = 0
    if (k === JSON_TOK_OBJECT_BEGIN) {
      k = t.next()
      while (k === JSON_TOK_KEY) {
        const isBody: boolean = t.keyEquals("body")
        const v: i32 = t.next()
        if (v < 0) {
          return BODY_MALFORMED
        }
        if (isBody) {
          found = v === JSON_TOK_STRING
          if (found) {
            const want: i32 = t.stringLength()
            if (want > toI32(this.text.length)) {
              this.text = new Array<u8>(want)
            }
            n = t.stringInto(this.text, 0)
          }
        }
        if (t.skipValue() < 0) {
          return BODY_MALFORMED
        }
        k = t.next()
      }
      if (k !== JSON_TOK_OBJECT_END) {
        return BODY_MALFORMED
      }
    } else if (t.skipValue() < 0) {
      return BODY_MALFORMED
    }
    // The whole text must be one value: anything after it is malformed.
    if (t.next() !== 0 || t.failed !== 0) {
      return BODY_MALFORMED
    }
    if (!found || n < 0) {
      return BODY_REQUIRED
    }
    const b: u8[] = this.text
    let s: i32 = 0
    let e: i32 = n
    while (s < e) {
      const w: i32 = spaceAt(b, s, e)
      if (w === 0) {
        break
      }
      s += w
    }
    while (e > s) {
      const w: i32 = spaceBefore(b, s, e)
      if (w === 0) {
        break
      }
      e -= w
    }
    if (s === e) {
      return BODY_REQUIRED
    }
    // Code points: every byte but a UTF-8 continuation byte starts one.
    let chars: i32 = 0
    for (let k2: i32 = s; k2 < e; k2++) {
      if ((toI32(b[k2]) & 0xc0) !== 0x80) {
        chars++
      }
    }
    if (chars > BODY_MAX_CHARS) {
      return BODY_TOO_LONG
    }
    this.start = s
    this.end = e
    return BODY_OK
  }
}
