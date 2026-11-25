// The response bodies, written straight into a caller's byte array: compact
// JSON with SPEC.md's key order, strings escaped where JSON requires it and
// copied sixteen bytes at a time between escapes (`jsonPutString`). A writer never grows its array; `failed` says it ran out.
import { copyBytes } from "nish/bytes"
import { jsonPutString } from "nish/json"
import { HEAD_ROOM } from "./http"

/** The ASCII `s` as bytes. */
export const bytesOf = (s: string): u8[] => {
  const b: u8[] = new Array<u8>(s.length)
  for (let k: i32 = 0; k < toI32(s.length); k++) {
    b[k] = toU8(s.charCodeAt(k))
  }
  return b
}

/** Appends to `buf` from 0; `end` is where the next byte goes. */
export class Out {
  buf: u8[]
  /** Where the body starts: 0, or `HEAD_ROOM` after `open`. */
  start: i32 = 0
  end: i32 = 0
  failed: boolean = false

  constructor(size: i32) {
    this.buf = new Array<u8>(size)
  }

  reset(): void {
    this.start = 0
    this.end = 0
    this.failed = false
  }

  /** Empties the writer for a response body, keeping `HEAD_ROOM` bytes in front of it for the head. */
  open(): void {
    this.start = HEAD_ROOM
    this.end = HEAD_ROOM
    this.failed = false
  }

  /** Whether `buf` holds `n` more bytes, growing it if not; the check alone is small enough to inline. */
  room(n: i32): boolean {
    return this.end + n <= toI32(this.buf.length) || this.grow(n)
  }

  /** Grows `buf` to hold `n` more bytes, keeping what is written. */
  grow(n: i32): boolean {
    let size: i32 = toI32(this.buf.length) * 2
    while (size < this.end + n) {
      size = size * 2
    }
    if (size > 16777216) {
      this.failed = true
      return false
    }
    const next: u8[] = new Array<u8>(size)
    copyBytes(next, 0, this.buf, 0, this.end)
    this.buf = next
    return true
  }

  bytes(src: readonly u8[], off: i32, len: i32): void {
    if (len > 0 && this.room(len)) {
      copyBytes(this.buf, this.end, src, off, len)
      this.end += len
    }
  }

  byte(c: i32): void {
    if (this.room(1)) {
      this.buf[this.end] = toU8(c)
      this.end += 1
    }
  }

  /** `v` in decimal. */
  int(v: i64): void {
    if (!this.room(21)) {
      return
    }
    const buf: u8[] = this.buf
    let o: i32 = this.end
    let n: i64 = v
    if (n < 0) {
      buf[o] = toU8(45)
      o++
      n = -n
    }
    // The digits are counted by comparison, then written last to first with
    // one division each.
    let digits: i32 = 1
    let bound: i64 = 10
    while (digits < 19 && n >= bound) {
      digits++
      if (digits < 19) {
        bound = bound * 10
      }
    }
    let k: i32 = o + digits - 1
    while (n >= 10) {
      const q: i64 = n / 10
      buf[k] = toU8(48 + toI32(n - q * 10))
      n = q
      k--
    }
    buf[k] = toU8(48 + toI32(n))
    this.end = o + digits
  }

  /** `src[off, off + len)`, UTF-8, as a JSON string with its quotes. */
  string(src: u8[], off: i32, len: i32): void {
    let e: i32 = jsonPutString(this.buf, this.end, src, off, len)
    if (e < 0) {
      // Each byte becomes at most six (\u00XX), and two quotes.
      if (!this.room(len * 6 + 2)) {
        return
      }
      e = jsonPutString(this.buf, this.end, src, off, len)
      if (e < 0) {
        this.failed = true
        return
      }
    }
    this.end = e
  }
}
