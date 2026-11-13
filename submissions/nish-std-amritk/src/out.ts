// The response bodies, written straight into a caller's byte array: compact
// JSON with SPEC.md's key order, strings escaped where JSON requires it and
// copied in one piece between escapes (`skipJsonPlain` finds the next byte
// that needs one). A writer never grows its array; `failed` says it ran out.
import { copyBytes, skipJsonPlain } from "nish/bytes"

const HEX: string = "0123456789abcdef"

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
  end: i32 = 0
  failed: boolean = false

  constructor(size: i32) {
    this.buf = new Array<u8>(size)
  }

  reset(): void {
    this.end = 0
    this.failed = false
  }

  /** Grows `buf` to hold `n` more bytes, keeping what is written. */
  room(n: i32): boolean {
    if (this.end + n <= toI32(this.buf.length)) {
      return true
    }
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
    let n: i64 = v
    if (n < 0) {
      this.buf[this.end] = toU8(45)
      this.end += 1
      n = -n
    }
    let digits: i32 = 1
    let probe: i64 = n
    while (probe >= 10) {
      probe = probe / 10
      digits++
    }
    for (let k: i32 = digits - 1; k >= 0; k--) {
      this.buf[this.end + k] = toU8(48 + toI32(n % 10))
      n = n / 10
    }
    this.end += digits
  }

  /** `src[off, off + len)`, UTF-8, as a JSON string with its quotes. */
  string(src: u8[], off: i32, len: i32): void {
    // Each byte becomes at most six (\u00XX), and two quotes.
    if (!this.room(len * 6 + 2)) {
      return
    }
    const buf: u8[] = this.buf
    let o: i32 = this.end
    buf[o] = toU8(34)
    o++
    let k: i32 = off
    const stop: i32 = off + len
    while (k < stop) {
      const plain: i32 = skipJsonPlain(src, k, stop)
      if (plain > k) {
        copyBytes(buf, o, src, k, plain - k)
        o += plain - k
        k = plain
        if (k >= stop) {
          break
        }
      }
      const c: i32 = toI32(src[k])
      buf[o] = toU8(92)
      if (c === 34 || c === 92) {
        buf[o + 1] = toU8(c)
        o += 2
      } else if (c === 10) {
        buf[o + 1] = toU8(110)
        o += 2
      } else if (c === 13) {
        buf[o + 1] = toU8(114)
        o += 2
      } else if (c === 9) {
        buf[o + 1] = toU8(116)
        o += 2
      } else if (c === 8) {
        buf[o + 1] = toU8(98)
        o += 2
      } else if (c === 12) {
        buf[o + 1] = toU8(102)
        o += 2
      } else if (c < 32) {
        buf[o + 1] = toU8(117)
        buf[o + 2] = toU8(48)
        buf[o + 3] = toU8(48)
        buf[o + 4] = toU8(HEX.charCodeAt(c >> 4))
        buf[o + 5] = toU8(HEX.charCodeAt(c & 15))
        o += 6
      } else {
        // A byte skipJsonPlain stops at that JSON leaves alone.
        buf[o] = toU8(c)
        o += 1
      }
      k++
    }
    buf[o] = toU8(34)
    this.end = o + 1
  }
}
