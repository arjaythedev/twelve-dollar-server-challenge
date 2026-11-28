// HTTP/1.1 for this server alone: the request heads it reads and the response
// heads it writes, over `nish:net`'s descriptors and ring slots directly.
//
// A request's head is parsed where it lies, in the buffer it was read into,
// so a connection between requests holds no buffer at all; a connection's
// state is one 32-byte row of `Conns.state`. A head's lines are found with
// `skipJsonPlain`, whose kernel reads sixteen bytes at a time and stops at
// every control byte, CR and LF among them. A response's head is written in
// front of its body, into room the body's buffer keeps there (`HEAD_ROOM`),
// so head and body leave in one `netWrite`.
import { copyBytes, skipJsonPlain } from "nish/bytes"

/** Bytes a response buffer keeps free in front of its body for the head. */
export const HEAD_ROOM: i32 = 160

/** The longest head read; a longer one is refused. */
const MAX_HEAD: i32 = 16384
/** The longest body read; a longer one is refused. */
export const MAX_BODY: i32 = 1048576

export const M_OTHER: i32 = 0
export const M_GET: i32 = 1
export const M_POST: i32 = 2

/** `parseHead` found no whole head yet. */
export const HEAD_MORE: i32 = 0
/** `parseHead` found a head it refuses: malformed, too long, or chunked. */
export const HEAD_BAD: i32 = -1

const CR: i32 = 13
const LF: i32 = 10
const SPACE: i32 = 32
const TAB: i32 = 9
const COLON: i32 = 58

/** Whether `b[at, at + s.length)`, ASCII-lowercased, is the lowercase `s`. */
const lowerIs = (b: u8[], at: i32, s: string): boolean => {
  const n: i32 = toI32(s.length)
  for (let k: i32 = 0; k < n; k++) {
    if ((toI32(b[at + k]) | 32) !== s.charCodeAt(k)) {
      return false
    }
  }
  return true
}

/** The index of the LF that ends the line from `i`, or -1 when `[i, to)` holds none. */
const lineEnd = (b: u8[], i: i32, to: i32): i32 => {
  let k: i32 = i
  while (k < to) {
    const j: i32 = skipJsonPlain(b, k, to)
    if (j >= to) {
      return -1
    }
    if (toI32(b[j]) === LF) {
      return j
    }
    k = j + 1
  }
  return -1
}

/** A request head's parts, as spans into the buffer it was parsed in. */
export class Head {
  method: i32 = 0
  targetStart: i32 = 0
  targetEnd: i32 = 0
  /** The Authorization value, trimmed; -1 when there is none. */
  authStart: i32 = -1
  authEnd: i32 = -1
  contentLength: i32 = 0
  keepAlive: boolean = true
}

/**
 * Parses the head that starts at `b[from]` into `h`: the index past its
 * blank line, `HEAD_MORE` when `[from, to)` does not hold all of it yet, or
 * `HEAD_BAD`. Lines end with LF, a CR before it dropped. HTTP/1.0 closes
 * after its answer unless it asks to keep alive; `Connection: close` closes
 * either. A Transfer-Encoding is refused: no client of this API sends one.
 */
export const parseHead = (b: u8[], from: i32, to: i32, h: Head): i32 => {
  const first: i32 = lineEnd(b, from, to)
  if (first < 0) {
    return to - from > MAX_HEAD ? HEAD_BAD : HEAD_MORE
  }
  // The request line: method SP target SP HTTP/1.x
  let e: i32 = first
  if (e > from && toI32(b[e - 1]) === CR) {
    e--
  }
  let sp: i32 = from
  while (sp < e && toI32(b[sp]) !== SPACE) {
    sp++
  }
  const m: i32 = sp - from
  h.method =
    m === 3 && toI32(b[from]) === 71 && toI32(b[from + 1]) === 69 && toI32(b[from + 2]) === 84
      ? M_GET
      : m === 4 && toI32(b[from]) === 80 && toI32(b[from + 1]) === 79 && toI32(b[from + 2]) === 83 && toI32(b[from + 3]) === 84
        ? M_POST
        : M_OTHER
  const ts: i32 = sp + 1
  let te: i32 = ts
  while (te < e && toI32(b[te]) !== SPACE) {
    te++
  }
  // "HTTP/1.x" after the target.
  if (m === 0 || te === ts || e - te !== 9 || toI32(b[te + 1]) !== 72 || toI32(b[te + 6]) !== 49 || toI32(b[te + 7]) !== 46) {
    return HEAD_BAD
  }
  h.targetStart = ts
  h.targetEnd = te
  h.keepAlive = toI32(b[te + 8]) !== 48
  h.authStart = -1
  h.authEnd = -1
  h.contentLength = 0
  let i: i32 = first + 1
  while (true) {
    const lf: i32 = lineEnd(b, i, to)
    if (lf < 0) {
      return to - from > MAX_HEAD ? HEAD_BAD : HEAD_MORE
    }
    let le: i32 = lf
    if (le > i && toI32(b[le - 1]) === CR) {
      le--
    }
    if (le === i) {
      return lf - from + 1 > MAX_HEAD ? HEAD_BAD : lf + 1
    }
    let colon: i32 = i
    while (colon < le && toI32(b[colon]) !== COLON) {
      colon++
    }
    if (colon === le || colon === i) {
      return HEAD_BAD
    }
    let vs: i32 = colon + 1
    while (vs < le && (toI32(b[vs]) === SPACE || toI32(b[vs]) === TAB)) {
      vs++
    }
    let ve: i32 = le
    while (ve > vs && (toI32(b[ve - 1]) === SPACE || toI32(b[ve - 1]) === TAB)) {
      ve--
    }
    const n: i32 = colon - i
    const c0: i32 = toI32(b[i]) | 32
    if (n === 13 && c0 === 97 && lowerIs(b, i, "authorization")) {
      h.authStart = vs
      h.authEnd = ve
    } else if (n === 14 && c0 === 99 && lowerIs(b, i, "content-length")) {
      if (ve === vs || ve - vs > 9) {
        return HEAD_BAD
      }
      let v: i32 = 0
      for (let k: i32 = vs; k < ve; k++) {
        const d: i32 = toI32(b[k]) - 48
        if (d < 0 || d > 9) {
          return HEAD_BAD
        }
        v = v * 10 + d
      }
      if (v > MAX_BODY) {
        return HEAD_BAD
      }
      h.contentLength = v
    } else if (n === 10 && c0 === 99 && lowerIs(b, i, "connection")) {
      if (ve - vs === 5 && lowerIs(b, vs, "close")) {
        h.keepAlive = false
      } else if (ve - vs === 10 && lowerIs(b, vs, "keep-alive")) {
        h.keepAlive = true
      }
    } else if (n === 17 && c0 === 116 && lowerIs(b, i, "transfer-encoding")) {
      return HEAD_BAD
    }
    i = lf + 1
  }
}

/**
 * The response heads' fixed parts, encoded once, and the writer that puts a
 * head in front of a body.
 */
export class Heads {
  statusLines: u8[][]
  statusCodes: i32[]
  /** `\r\nContent-Type: application/json\r\nContent-Length: ` */
  typeAndLength: u8[]
  dateKey: u8[]
  /** What ends a head: its blank line, after `Connection: close` for a closing one. */
  plainTail: u8[]
  closeTail: u8[]

  constructor() {
    this.statusCodes = [200, 201, 400, 401, 404, 500, 503]
    const lines: string[] = [
      "HTTP/1.1 200 OK",
      "HTTP/1.1 201 Created",
      "HTTP/1.1 400 Bad Request",
      "HTTP/1.1 401 Unauthorized",
      "HTTP/1.1 404 Not Found",
      "HTTP/1.1 500 Internal Server Error",
      "HTTP/1.1 503 Service Unavailable",
    ]
    this.statusLines = []
    for (let k: i32 = 0; k < toI32(lines.length); k++) {
      this.statusLines.push(ascii(lines[k]))
    }
    this.typeAndLength = ascii("\r\nContent-Type: application/json\r\nContent-Length: ")
    this.dateKey = ascii("\r\nDate: ")
    this.plainTail = ascii("\r\n\r\n")
    this.closeTail = ascii("\r\nConnection: close\r\n\r\n")
  }

  /**
   * Writes the head for a `status` answer whose `len`-byte body starts at
   * `buf[start]` into the `HEAD_ROOM` bytes before it, and answers where the
   * head starts. `date` is the 29-byte IMF-fixdate. Its length is known
   * before a byte is written, so each part is copied once, into place.
   */
  write(buf: u8[], start: i32, status: i32, len: i32, date: u8[], close: boolean): i32 {
    let line: i32 = 5
    for (let k: i32 = 0; k < toI32(this.statusCodes.length); k++) {
      if (this.statusCodes[k] === status) {
        line = k
        break
      }
    }
    const sl: u8[] = this.statusLines[line]
    let digits: i32 = 1
    let v: i32 = len
    while (v >= 10) {
      v = v / 10
      digits++
    }
    const tail: u8[] = close ? this.closeTail : this.plainTail
    const n: i32 = toI32(sl.length) + toI32(this.typeAndLength.length) + digits + toI32(this.dateKey.length) + toI32(date.length) + toI32(tail.length)
    const at: i32 = start - n
    let o: i32 = at
    copyBytes(buf, o, sl, 0, toI32(sl.length))
    o += toI32(sl.length)
    copyBytes(buf, o, this.typeAndLength, 0, toI32(this.typeAndLength.length))
    o += toI32(this.typeAndLength.length)
    v = len
    for (let k: i32 = o + digits - 1; k >= o; k--) {
      buf[k] = toU8(48 + (v % 10))
      v = v / 10
    }
    o += digits
    copyBytes(buf, o, this.dateKey, 0, toI32(this.dateKey.length))
    o += toI32(this.dateKey.length)
    copyBytes(buf, o, date, 0, toI32(date.length))
    o += toI32(date.length)
    copyBytes(buf, o, tail, 0, toI32(tail.length))
    return at
  }
}

const ascii = (s: string): u8[] => {
  const b: u8[] = new Array<u8>(s.length)
  for (let k: i32 = 0; k < toI32(s.length); k++) {
    b[k] = toU8(s.charCodeAt(k))
  }
  return b
}

/** A connection row's fields, in `Conns.state`. */
export const C_FD: i32 = 0
export const C_GEN: i32 = 1
/** The request waiting for its group's commit, or -1. */
export const C_REQ: i32 = 2
export const C_FLAGS: i32 = 3
/** Input kept between reads: a head or body not yet whole, or requests behind a waiting one. */
export const C_PEND: i32 = 4
/** Output the socket did not take yet: `out[c][OUT_OFF, OUT_LEN)`. */
export const C_OUT_OFF: i32 = 5
export const C_OUT_LEN: i32 = 6
export const C_STRIDE: i32 = 8

/** `C_FLAGS`: close once the answer is out. */
export const F_CLOSE: i32 = 1
/** `C_FLAGS`: the loop watches the socket for room to write. */
export const F_WRITE: i32 = 2

/**
 * Every connection's state, by index: one row of `C_STRIDE` i32s each, so a
 * request reads and writes one cache line of it. A connection's kept input
 * and output live in `pend` and `out` only while it has any.
 */
export class Conns {
  state: i32[]
  pend: u8[][]
  out: u8[][]
  free: i32[]
  empty: u8[]
  size: i32

  constructor(size: i32) {
    this.size = size
    this.state = new Array<i32>(size * C_STRIDE)
    this.empty = []
    this.pend = []
    this.out = []
    this.free = []
    for (let c: i32 = 0; c < size; c++) {
      this.state[c * C_STRIDE + C_FD] = -1
      this.state[c * C_STRIDE + C_REQ] = -1
      this.pend.push(this.empty)
      this.out.push(this.empty)
    }
    for (let c: i32 = size - 1; c >= 0; c--) {
      this.free.push(c)
    }
  }

  get(c: i32, f: i32): i32 {
    return this.state[c * C_STRIDE + f]
  }

  set(c: i32, f: i32, v: i32): void {
    this.state[c * C_STRIDE + f] = v
  }
}

/** The size of a pooled buffer; a larger one is pooled apart, a few at a time. */
export const POOL_BUF: i32 = 4096
const POOL_BIG_MAX: i32 = 64

/**
 * The buffers connections keep input and output in while they have any.
 * There is no garbage collector: a buffer stored in a connection's row and
 * then dropped would stay allocated for good, so every one comes from here
 * and goes back here, last in, first out.
 */
export class BufPool {
  small: u8[][]
  big: u8[][]

  constructor() {
    this.small = []
    this.big = []
  }

  /** A buffer of at least `n` bytes. */
  take(n: i32): u8[] {
    if (n <= POOL_BUF) {
      if (this.small.length > 0) {
        const b: u8[] = this.small[this.small.length - 1]
        this.small.pop()
        return b
      }
      return new Array<u8>(POOL_BUF)
    }
    for (let k: i32 = toI32(this.big.length) - 1; k >= 0; k--) {
      const b: u8[] = this.big[k]
      if (toI32(b.length) >= n) {
        this.big[k] = this.big[this.big.length - 1]
        this.big.pop()
        return b
      }
    }
    let size: i32 = POOL_BUF * 2
    while (size < n) {
      size = size * 2
    }
    return new Array<u8>(size)
  }

  /** `b` back for reuse; an empty one is nobody's. */
  give(b: u8[]): void {
    if (b.length === 0) {
      return
    }
    if (toI32(b.length) === POOL_BUF) {
      this.small.push(b)
    } else if (toI32(this.big.length) < POOL_BIG_MAX) {
      this.big.push(b)
    }
  }
}
