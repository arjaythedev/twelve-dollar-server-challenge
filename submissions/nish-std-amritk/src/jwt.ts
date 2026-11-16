// The challenge's tokens: HS256 JWTs whose payload is
// {"sub":"<user id>","username":"<name>","iat":…,"exp":…} (SPEC.md, "Auth").
// Every request's token has its signature, its `alg` and its `exp` checked
// here; nothing is remembered from one request to the next (rule 5).
//
// The key's two pads are absorbed once (`keyed`), and each signature copies
// the two keyed SHA-256 states and feeds them the message, so a token costs
// the message's blocks and two finishing blocks, not the key's two again.
import { timingSafeEqualAt } from "nish/crypto/ct"
import { HmacSha256, hmacCopySha256 } from "nish/crypto/hmac"
import {
  JSON_TOK_KEY,
  JSON_TOK_NUMBER,
  JSON_TOK_OBJECT_BEGIN,
  JSON_TOK_OBJECT_END,
  JSON_TOK_STRING,
  JsonLimits,
  JsonTokenizer,
} from "nish/json-tokenizer"

/** `verify`'s answers: a usable token, and SPEC.md's two kinds of refusal. */
export const JWT_OK: i32 = 0
export const JWT_INVALID: i32 = 1
export const JWT_BAD_PAYLOAD: i32 = 2

/** The longest token `verify` reads; a longer one is refused unread. */
const JWT_MAX_TOKEN: i32 = 4096
/** The longest `username` a token may carry. */
export const JWT_MAX_NAME: i32 = 1024
/** base64url of `{"alg":"HS256","typ":"JWT"}`, the header nearly every token has. */
const JWT_HEADER: string = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"

/** The sextet base64url gives the byte `c`, or -1 when `c` is not in its alphabet. */
const jwtSextet = (c: i32): i32 => {
  if (c >= 65 && c <= 90) {
    return c - 65
  }
  if (c >= 97 && c <= 122) {
    return c - 71
  }
  if (c >= 48 && c <= 57) {
    return c + 4
  }
  if (c === 45) {
    return 62
  }
  if (c === 95) {
    return 63
  }
  return -1
}

/**
 * Decodes unpadded base64url `src[off, off + len)` into `out` from 0, and
 * answers the bytes written; -1 for a byte outside the alphabet, a length no
 * encoding has (1 mod 4), stray bits in the last sextet, or no room.
 */
const jwtDecode = (src: u8[], off: i32, len: i32, out: u8[]): i32 => {
  if (off < 0 || len < 0 || off + len > toI32(src.length) || len % 4 === 1) {
    return -1
  }
  if ((len * 3) >> 2 > toI32(out.length)) {
    return -1
  }
  let acc: i32 = 0
  let bits: i32 = 0
  let o: i32 = 0
  for (let k: i32 = off; k < off + len; k++) {
    const v: i32 = jwtSextet(toI32(src[k]))
    if (v < 0) {
      return -1
    }
    acc = ((acc << 6) | v) & 0xffffff
    bits += 6
    if (bits >= 8) {
      bits -= 8
      out[o] = toU8((acc >> bits) & 255)
      o++
    }
  }
  // RFC 4648 §3.5: the bits past the last whole byte are zero in a canonical encoding.
  if ((acc & ((1 << bits) - 1)) !== 0) {
    return -1
  }
  return o
}

/** The verifier of one secret's tokens. */
export class Jwt {
  /** The two pads, absorbed once; `work` starts each signature from copies of them. */
  keyed: HmacSha256
  work: HmacSha256
  /** A decoded header or payload, and a decoded signature. */
  part: u8[]
  sig: u8[]
  tokenizer: JsonTokenizer
  /** `JWT_HEADER`'s bytes. */
  header: u8[]
  /** What the last good `verify` found: the user id, and the username's bytes in `name[0, nameLength)`. */
  sub: i64 = 0
  name: u8[]
  nameLength: i32 = 0

  constructor(secret: u8[]) {
    this.keyed = new HmacSha256(secret)
    this.work = new HmacSha256(secret)
    this.part = new Array<u8>(JWT_MAX_TOKEN)
    this.sig = new Array<u8>(64)
    this.name = new Array<u8>(JWT_MAX_NAME)
    this.header = new Array<u8>(JWT_HEADER.length)
    for (let k: i32 = 0; k < toI32(JWT_HEADER.length); k++) {
      this.header[k] = toU8(JWT_HEADER.charCodeAt(k))
    }
    const limits = new JsonLimits()
    limits.maxDepth = 8
    this.tokenizer = new JsonTokenizer(this.part, 0, 0, limits)
  }

  /** The HMAC-SHA-256 of `src[off, off + len)` written at `out[at]`. */
  sign(src: u8[], off: i32, len: i32, out: u8[], at: i32): void {
    hmacCopySha256(this.keyed.inner, this.work.inner)
    hmacCopySha256(this.keyed.outer, this.work.outer)
    this.work.update(src, off, len)
    this.work.digestInto(out, at)
  }

  /**
   * Checks the token `src[off, off + len)` at `now` (seconds): `JWT_OK` with
   * `sub` and `name` set; `JWT_INVALID` for a malformed token, a bad
   * signature, an `alg` other than HS256, or an `exp` that is missing or
   * past; `JWT_BAD_PAYLOAD` for a good token whose `sub` is not a positive
   * integer string or whose `username` is not a string.
   */
  verify(src: u8[], off: i32, len: i32, now: i64): i32 {
    if (len <= 0 || len > JWT_MAX_TOKEN || off < 0 || off + len > toI32(src.length)) {
      return JWT_INVALID
    }
    let first: i32 = -1
    let second: i32 = -1
    for (let k: i32 = off; k < off + len; k++) {
      if (toI32(src[k]) === 46) {
        if (first < 0) {
          first = k
        } else if (second < 0) {
          second = k
        } else {
          return JWT_INVALID
        }
      }
    }
    if (first < 0 || second < 0) {
      return JWT_INVALID
    }
    if (jwtDecode(src, second + 1, off + len - second - 1, this.sig) !== 32) {
      return JWT_INVALID
    }
    this.sign(src, off, second - off, this.part, 0)
    if (!timingSafeEqualAt(this.part, 0, this.sig, 0, 32)) {
      return JWT_INVALID
    }
    if (!this.isUsualHeader(src, off, first - off)) {
      const headerLen: i32 = jwtDecode(src, off, first - off, this.part)
      if (headerLen < 0 || !this.headerIsHs256(headerLen)) {
        return JWT_INVALID
      }
    }
    const payloadLen: i32 = jwtDecode(src, first + 1, second - first - 1, this.part)
    if (payloadLen < 0) {
      return JWT_INVALID
    }
    return this.claims(payloadLen, now)
  }

  /** Whether `src[off, off + len)` is exactly `JWT_HEADER`. */
  isUsualHeader(src: u8[], off: i32, len: i32): boolean {
    const h: u8[] = this.header
    if (len !== toI32(h.length)) {
      return false
    }
    for (let k: i32 = 0; k < len; k++) {
      if (src[off + k] !== h[k]) {
        return false
      }
    }
    return true
  }

  /** Whether the header in `part[0, len)` is an object whose `alg` is exactly `HS256`. */
  headerIsHs256(len: i32): boolean {
    const t: JsonTokenizer = this.tokenizer
    t.reset(this.part, 0, len)
    if (t.next() !== JSON_TOK_OBJECT_BEGIN) {
      return false
    }
    let alg: boolean = false
    let k: i32 = t.next()
    while (k === JSON_TOK_KEY) {
      if (t.keyEquals("alg")) {
        // The last `alg` is the one that counts, as in JSON.parse.
        alg = t.next() === JSON_TOK_STRING && t.keyEquals("HS256")
      } else if (t.skipValue() < 0) {
        return false
      }
      k = t.next()
    }
    return k === JSON_TOK_OBJECT_END && alg && t.next() === 0 && t.failed === 0
  }

  /** Reads the payload in `part[0, len)`: `exp` first decides validity, then `sub` and `username` the payload's. */
  claims(len: i32, now: i64): i32 {
    const t: JsonTokenizer = this.tokenizer
    t.reset(this.part, 0, len)
    if (t.next() !== JSON_TOK_OBJECT_BEGIN) {
      return JWT_INVALID
    }
    let sub: i64 = -1
    let exp: i64 = -1
    let hasExp: boolean = false
    let named: boolean = false
    let k: i32 = t.next()
    while (k === JSON_TOK_KEY) {
      // The last of a repeated key is the one that counts, as in JSON.parse.
      const which: i32 = t.keyEquals("sub") ? 1 : t.keyEquals("username") ? 2 : t.keyEquals("exp") ? 3 : 0
      const v: i32 = t.next()
      if (v < 0) {
        return JWT_INVALID
      }
      if (which === 1) {
        sub = v === JSON_TOK_STRING ? this.decimal(t.tokStart + 1, t.tokEnd - 1) : -1
      } else if (which === 2) {
        named = false
        if (v === JSON_TOK_STRING && t.stringLength() <= JWT_MAX_NAME) {
          this.nameLength = t.stringInto(this.name, 0)
          named = this.nameLength >= 0
        }
      } else if (which === 3) {
        hasExp = v === JSON_TOK_NUMBER && t.isInteger()
        exp = hasExp ? t.numberI64() : -1
      }
      if (t.skipValue() < 0) {
        return JWT_INVALID
      }
      k = t.next()
    }
    if (k !== JSON_TOK_OBJECT_END || t.next() !== 0 || t.failed !== 0) {
      return JWT_INVALID
    }
    if (!hasExp || exp <= now) {
      return JWT_INVALID
    }
    if (sub <= 0 || !named) {
      return JWT_BAD_PAYLOAD
    }
    this.sub = sub
    return JWT_OK
  }

  /** `part[start, end)` read as 1 to 15 decimal digits, no leading zero; -1 otherwise. */
  decimal(start: i32, end: i32): i64 {
    if (end <= start || end - start > 15 || toI32(this.part[start]) === 48) {
      return -1
    }
    let v: i64 = 0
    for (let k: i32 = start; k < end; k++) {
      const d: i32 = toI32(this.part[k]) - 48
      if (d < 0 || d > 9) {
        return -1
      }
      v = v * 10 + toI64(d)
    }
    return v
  }
}
