// One serving process: a single-threaded `pollWait` loop over its listener
// and its keep-alive connections, whose HTTP/1.1 is `./http` over `nish:net`,
// and one SQLite connection (`nish/sqlite`) that every request reads and
// writes inline.
//
// A pass of the loop is everything one `pollWait` hands back. Reads are
// answered as their request ends, each statement reading SQLite while that
// request is served. Writes wait for the end of the pass and then go as one
// group: one `BEGIN IMMEDIATE`, each write, one `COMMIT` (WAL,
// synchronous=NORMAL). Each write's response is sent only after that
// `COMMIT` returns (rule 6); if it fails, every write in the group answers
// 500 and none is retried, since none committed.
import { netClose, netRead, netWrite, pollAdd, pollCreate, pollModify, pollWait, tcpAccept } from "nish:net"
import { monotonicNanos } from "nish:process"
import { copyBytes } from "nish/bytes"
import { HttpDate } from "nish/net/http1-server"
import { Cursor, Database, RowRecord, SQLITE_BUSY, SQLITE_INTEGER, SQLITE_TEXT, Statement } from "nish/sqlite"
import { BODY_MALFORMED, BODY_OK, BODY_REQUIRED, BodyReader } from "./body"
import { JWT_BAD_PAYLOAD, JWT_OK, Jwt } from "./jwt"
import { Out, bytesOf } from "./out"
import {
  C_FD,
  C_FLAGS,
  C_GEN,
  C_OUT_LEN,
  C_OUT_OFF,
  C_PEND,
  C_REQ,
  BufPool,
  Conns,
  F_CLOSE,
  F_WRITE,
  HEAD_BAD,
  HEAD_MORE,
  Head,
  Heads,
  M_GET,
  M_POST,
  parseHead,
} from "./http"

const R_NONE: i32 = 0
const R_HEALTH: i32 = 1
const R_FEED: i32 = 2
const R_POST: i32 = 3
const R_CREATE: i32 = 4
const R_LIKE: i32 = 5

/** `pollWait`'s token for the listener; a connection's is its index in `conns`. */
const TOKEN_LISTENER: i32 = -1
/** Readable, or hung up: `pollWait`'s bits 1 and 4. */
const READABLE: i32 = 5
const WRITABLE: i32 = 2
/** The interest `pollAdd` and `pollModify` take: read, and write. */
const WANT_READ: i32 = 1
const WANT_WRITE: i32 = 2

/** Status codes and SPEC.md's error messages, by index in `errors`. */
const E_NOT_FOUND: i32 = 0
const E_BAD_ID: i32 = 1
const E_NO_POST: i32 = 2
const E_NO_TOKEN: i32 = 3
const E_BAD_TOKEN: i32 = 4
const E_BAD_PAYLOAD: i32 = 5
const E_MALFORMED: i32 = 6
const E_REQUIRED: i32 = 7
const E_TOO_LONG: i32 = 8
const E_INTERNAL: i32 = 9

/** The widest id range the feed reads as ranges; wider, the reference query answers. */
const RANGE_MAX: i32 = 256

const POST_SQL: string =
  "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id"

/** The server's configuration, from the environment (main.ts). */
export class ServerConfig {
  sqlitePath: string = ""
  jwtSecret: string = ""
  host: string = "0.0.0.0"
  port: i32 = 3000
  /** Connections this process holds at most. */
  slots: i32 = 60000
  cacheKib: i32 = 65536
  mmapBytes: i64 = 0
  /** A pass with fewer events than this is followed by a wait of `batchMs` (`BATCH_MIN`). */
  batchMin: i32 = 64
  /** How long requests gather after a small pass, in ms; 0 never waits (`BATCH_MS`). */
  batchMs: i32 = 1
  /** Read posts through B-tree cursors (`CURSORS`, on by default) rather than SQL. */
  cursors: boolean = true
}

/** One request, from its head to its answer. */
class Req {
  index: i32
  slot: i32 = -1
  gen: i32 = 0
  route: i32 = 0
  /** An error decided before the request's end, as an index in `errors`; -1 for none. */
  error: i32 = -1
  postId: i64 = 0
  userId: i64 = 0
  name: u8[]
  nameLength: i32 = 0
  body: u8[]
  bodyLength: i32 = 0
  /** A write's response, held until its group commits. */
  out: Out
  status: i32 = 0

  constructor(index: i32) {
    this.index = index
    this.name = new Array<u8>(64)
    this.body = new Array<u8>(1024)
    this.out = new Out(1024)
  }

  addBody(src: u8[], off: i32, len: i32): void {
    if (this.bodyLength + len > toI32(this.body.length)) {
      let size: i32 = toI32(this.body.length) * 2
      while (size < this.bodyLength + len) {
        size = size * 2
      }
      const next: u8[] = new Array<u8>(size)
      for (let k: i32 = 0; k < this.bodyLength; k++) {
        next[k] = this.body[k]
      }
      this.body = next
    }
    for (let k: i32 = 0; k < len; k++) {
      this.body[this.bodyLength + k] = src[off + k]
    }
    this.bodyLength += len
  }
}

/**
 * Resets `st` for its next use. Its answer repeats the last step's error,
 * which the caller has already read, so it is dropped here.
 */
const settle = (st: Statement, _stepped: boolean = true): void => {
  const r = st.reset()
  if (r.isErr()) {
    return
  }
}

/** Whether `b[start, end)` is the ASCII `s`. */
const bytesAre = (b: u8[], start: i32, end: i32, s: string): boolean => {
  if (end - start !== toI32(s.length)) {
    return false
  }
  for (let k: i32 = 0; k < end - start; k++) {
    if (toI32(b[start + k]) !== toI32(s.charCodeAt(k))) {
      return false
    }
  }
  return true
}

/** `b[start, end)` as a positive decimal id, or -1: SPEC.md's "invalid post id" for anything else. */
const idOf = (b: u8[], start: i32, end: i32): i64 => {
  if (end <= start || end - start > 15) {
    return -1
  }
  let v: i64 = 0
  for (let k: i32 = start; k < end; k++) {
    const d: i32 = toI32(b[k]) - 48
    if (d < 0 || d > 9) {
      return -1
    }
    v = v * 10 + toI64(d)
  }
  return v > 0 ? v : -1
}

export class FeedServer {
  listener: i32
  conns: Conns
  /** Every buffer a connection keeps comes from here and goes back. */
  pool: BufPool
  head: Head
  heads: Heads
  date: HttpDate
  /** Where a read lands, and the request heads in it are parsed. */
  scratch: u8[]
  peer: u8[]
  /** An error's answer, with room for its head. */
  errOut: Out
  badRequest: u8[]
  loop: i32
  db: Database
  feed: Statement
  post: Statement
  insertPost: Statement
  insertLike: Statement
  postExists: Statement
  ping: Statement
  begin: Statement
  commit: Statement
  beginRead: Statement
  newest: Statement
  postRange: Statement
  likeRange: Statement
  /** The feed's ids in order, each one's place by `id - lo`, and its object's span in `rows` and its like count. */
  feedIds: i64[]
  /** Like counts by post id from a range's first, from `countRange`. */
  likeSpan: i64[]
  rangeAt: i32[]
  rowStart: i32[]
  rowEnd: i32[]
  likes: i64[]
  rows: Out
  /** Whether the pass's read transaction is open. */
  reading: boolean = false
  batchMin: i32
  batchMs: i32
  /** Read through cursors at all (`ServerConfig.cursors`). */
  useCursors: boolean
  /**
   * The pass's cursors, opened by the first read that wants them and closed by
   * `endRead`: `posts_created_at_id_idx`, `posts`, `users` and
   * `likes_post_id_idx`.
   */
  newestCur: Cursor | null = null
  postCur: Cursor | null = null
  userCur: Cursor | null = null
  likeCur: Cursor | null = null
  /** Whether the four cursors are open in the pass's read transaction. */
  cursorsOpen: boolean = false
  rollback: Statement
  jwt: Jwt
  bodies: BodyReader
  reqs: Req[]
  free: i32[]
  /** The writes waiting for the end of the pass, by request index. */
  writes: i32[]
  /** The other half of `writes`, swapped in while a group is answered. */
  spare: i32[]
  /** Whether the last group found the write lock taken and waits for the next pass. */
  held: boolean = false
  /** The answer being written for a read. */
  out: Out
  /** A text column's bytes, between SQLite and `out`. */
  col: u8[]
  errorStatus: i32[]
  errorBodies: u8[][]
  startedNs: i64
  feedOpen: u8[]
  postOpen: u8[]
  degraded: u8[]
  healthy: u8[]
  createdOpen: u8[]
  createdClose: u8[]
  likedOpen: u8[]
  trueBytes: u8[]
  falseBytes: u8[]
  postIdKey: u8[]
  /** The post object's keys, in SPEC.md's order. */
  frag: u8[][]
  /** The pass's wall clock, in ms: the Date header and every token's `exp` check read it. */
  passMs: i64 = 0
  /** A post's record (id, user_id, body, created_at), then an author's (id, username). */
  postRec: RowRecord
  userRec: RowRecord

  constructor(cfg: ServerConfig, listener: i32, db: Database) {
    this.listener = listener
    this.conns = new Conns(cfg.slots)
    this.pool = new BufPool()
    this.head = new Head()
    this.heads = new Heads()
    this.date = new HttpDate()
    this.scratch = new Array<u8>(65536)
    this.peer = new Array<u8>(32)
    this.errOut = new Out(1024)
    this.badRequest = bytesOf('{"error":"bad request"}')
    this.loop = pollCreate()
    pollAdd(this.loop, listener, WANT_READ, TOKEN_LISTENER)
    this.db = db
    this.useCursors = cfg.cursors
    this.batchMin = cfg.batchMin
    this.batchMs = cfg.batchMs
    this.feed = db.prepare(`${POST_SQL} ORDER BY p.created_at DESC, p.id DESC LIMIT 20`).expect("feed")
    this.post = db.prepare(`${POST_SQL} WHERE p.id = ?1`).expect("post")
    this.insertPost = db
      .prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at")
      .expect("insert post")
    this.insertLike = db
      .prepare(
        "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING",
      )
      .expect("insert like")
    this.postExists = db.prepare("SELECT 1 FROM posts WHERE id = ?1").expect("post exists")
    this.ping = db.prepare("SELECT 1").expect("ping")
    this.begin = db.prepare("BEGIN IMMEDIATE").expect("begin")
    this.commit = db.prepare("COMMIT").expect("commit")
    this.beginRead = db.prepare("BEGIN").expect("begin read")
    this.newest = db.prepare("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20").expect("newest")
    this.postRange = db
      .prepare(
        "SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ?1 AND ?2",
      )
      .expect("post range")
    this.likeRange = db
      .prepare("SELECT post_id, count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id")
      .expect("like range")
    this.feedIds = new Array<i64>(20)
    this.likeSpan = new Array<i64>(RANGE_MAX)
    this.rangeAt = new Array<i32>(RANGE_MAX)
    for (let k: i32 = 0; k < RANGE_MAX; k++) {
      this.rangeAt[k] = -1
    }
    this.rowStart = new Array<i32>(20)
    this.rowEnd = new Array<i32>(20)
    this.likes = new Array<i64>(20)
    this.rows = new Out(65536)
    this.rollback = db.prepare("ROLLBACK").expect("rollback")
    this.jwt = new Jwt(bytesOf(cfg.jwtSecret))
    this.bodies = new BodyReader()
    this.reqs = []
    this.free = []
    this.writes = []
    this.spare = []
    this.out = new Out(65536)
    this.col = new Array<u8>(4096)
    this.errorStatus = [404, 400, 404, 401, 401, 401, 400, 400, 400, 500]
    const messages: string[] = [
      "not found",
      "invalid post id",
      "post not found",
      "missing bearer token",
      "invalid or expired token",
      "invalid token payload",
      "malformed JSON body",
      "body is required",
      "body must be at most 500 characters",
      "internal server error",
    ]
    this.errorBodies = []
    for (let k: i32 = 0; k < toI32(messages.length); k++) {
      this.errorBodies.push(bytesOf(`{"error":"${messages[k]}"}`))
    }
    this.feedOpen = bytesOf('{"posts":[')
    this.postOpen = bytesOf('{"post":')
    this.degraded = bytesOf('{"status":"degraded","db":"unreachable","error":')
    this.healthy = bytesOf('{"status":"ok","db":"ok","uptime_s":')
    this.createdOpen = bytesOf('{"post":{"id":')
    this.createdClose = bytesOf(',"like_count":0}}')
    this.likedOpen = bytesOf('{"liked":true,"already_liked":')
    this.trueBytes = bytesOf("true")
    this.falseBytes = bytesOf("false")
    this.postIdKey = bytesOf(',"post_id":')
    this.frag = [bytesOf('{"id":'), bytesOf(',"body":'), bytesOf(',"created_at":'), bytesOf(',"author":'), bytesOf(',"like_count":')]
    this.startedNs = monotonicNanos()
    this.postRec = new RowRecord(4096, 4)
    this.userRec = new RowRecord(256, 2)
  }

  // ---- The loop -------------------------------------------------------------------

  /** Serves until the process is stopped; answers 1 when the loop fails. */
  run(): i32 {
    const ready: i32[] = new Array<i32>(2 * 1024)
    // An empty loop: waiting on it sleeps for its whole timeout.
    const nap: i32 = pollCreate()
    const napReady: i32[] = new Array<i32>(2)
    while (true) {
      // A held write group retries within 1 ms.
      const n: i32 = pollWait(this.loop, ready, this.held ? 1 : 1000)
      if (n < 0 && n !== -4) {
        return 1
      }
      // The pass's two clock reads: every activity stamp and the Date header
      // of the pass take them, rather than each request reading both clocks.
      this.passMs = toI64(Date.now())
      this.date.pin(this.passMs)
      for (let k: i32 = 0; k < n; k++) {
        this.ready(ready[2 * k], ready[2 * k + 1])
      }
      if (this.reading) {
        this.endRead()
      }
      // Every pass tries the waiting writes, a group held by the write lock
      // included (`commitWrites` clears `held` once BEGIN IMMEDIATE takes
      // it). A group's answers can serve writes pipelined behind them, which
      // join the next group: it commits now rather than after the next
      // wait, unless the lock is taken.
      let groups: i32 = 0
      while (this.writes.length > 0 && groups < 8 && (groups === 0 || !this.held)) {
        this.commitWrites()
        if (this.reading) {
          this.endRead()
        }
        groups++
      }
      // After a small pass, with no write left waiting for the lock, let
      // requests gather for `batchMs` (the pass's responses are sent first),
      // so the next pass serves them together: fewer waits, wakes and system
      // calls a request, and one COMMIT for more writes. A wait on an empty
      // poll set sleeps; `pollWaitBatch`, which serves completions as they
      // come in during its window, measured dearer each way it was tried.
      if (this.batchMs > 0 && n < this.batchMin && this.writes.length === 0) {
        pollWait(nap, napReady, this.batchMs)
      }
    }
  }

  ready(token: i32, events: i32): void {
    if (token >= 0) {
      if (this.conns.get(token, C_FD) >= 0) {
        if ((events & WRITABLE) !== 0) {
          this.writable(token)
        }
        if ((events & READABLE) !== 0 && this.conns.get(token, C_FD) >= 0) {
          this.readable(token)
        }
      }
    } else if (token === TOKEN_LISTENER) {
      this.acceptAll()
    }
  }

  acceptAll(): void {
    const conns: Conns = this.conns
    while (true) {
      const fd: i32 = tcpAccept(this.listener, this.peer)
      if (fd < 0) {
        // -4 and -103/-71 (a connection gone before it was taken) try again.
        if (fd === -4 || fd === -103 || fd === -71) {
          continue
        }
        return
      }
      if (conns.free.length === 0) {
        netClose(fd)
        continue
      }
      const c: i32 = conns.free[conns.free.length - 1]
      conns.free.pop()
      conns.set(c, C_FD, fd)
      conns.set(c, C_REQ, -1)
      conns.set(c, C_FLAGS, 0)
      conns.set(c, C_PEND, 0)
      conns.set(c, C_OUT_OFF, 0)
      conns.set(c, C_OUT_LEN, 0)
      if (pollAdd(this.loop, fd, WANT_READ, c) < 0) {
        this.closeConn(c)
      }
    }
  }

  /**
   * Reads what `c`'s socket holds and serves every whole request in it, in
   * order. A head or body not yet whole, and requests behind a write that
   * waits for its group's commit, are kept in `conns.pend[c]`; anything else
   * is read and served where it lies, in `scratch`.
   */
  readable(c: i32): void {
    const conns: Conns = this.conns
    while (conns.get(c, C_FD) >= 0) {
      const kept: i32 = conns.get(c, C_PEND)
      let buf: u8[] = this.scratch
      let at: i32 = 0
      if (kept > 0) {
        buf = conns.pend[c]
        at = kept
        if (at + 4096 > toI32(buf.length)) {
          buf = this.grow(buf, at, at + 4096)
          conns.pend[c] = buf
        }
      }
      const want: i32 = toI32(buf.length) - at
      const n: i32 = netRead(conns.get(c, C_FD), buf, at, want)
      if (n <= 0) {
        if (n !== -11) {
          this.closeConn(c)
        }
        return
      }
      this.serve(c, buf, at + n)
      // A short read emptied the socket; readiness is level-triggered, so
      // more input wakes the loop again.
      if (n < want) {
        return
      }
    }
  }

  /** `buf[0, len)` copied into a pooled buffer of at least `size` bytes; `buf` goes back to the pool. */
  grow(buf: u8[], len: i32, size: i32): u8[] {
    const next: u8[] = this.pool.take(size)
    copyBytes(next, 0, buf, 0, len)
    this.pool.give(buf)
    return next
  }

  /**
   * Serves the whole requests in `buf[0, end)`, the input `c` has (its kept
   * bytes and what was just read), and keeps the rest in `conns.pend[c]`.
   */
  serve(c: i32, buf: u8[], end: i32): void {
    const conns: Conns = this.conns
    const h: Head = this.head
    let at: i32 = 0
    while (at < end && conns.get(c, C_REQ) < 0 && conns.get(c, C_FD) >= 0 && (conns.get(c, C_FLAGS) & F_CLOSE) === 0) {
      const he: i32 = parseHead(buf, at, end, h)
      if (he === HEAD_BAD) {
        this.refuse(c)
        return
      }
      if (he === HEAD_MORE) {
        break
      }
      const stop: i32 = he + h.contentLength
      if (stop > end) {
        break
      }
      if (!h.keepAlive) {
        conns.set(c, C_FLAGS, conns.get(c, C_FLAGS) | F_CLOSE)
      }
      this.request(c, buf, h, he, stop)
      at = stop
    }
    if (conns.get(c, C_FD) < 0) {
      return
    }
    const left: i32 = end - at
    const kept: i32 = conns.get(c, C_PEND)
    if (left === 0) {
      // `pend[c]` is touched only by a connection that kept bytes, so a
      // request served from `scratch` reads one cache line of `conns`.
      if (kept > 0) {
        conns.set(c, C_PEND, 0)
        this.pool.give(conns.pend[c])
        conns.pend[c] = conns.empty
      }
      return
    }
    let keep: u8[] = kept > 0 || buf !== this.scratch ? conns.pend[c] : conns.empty
    if (buf === keep) {
      if (at > 0) {
        copyBytes(keep, 0, keep, at, left)
      }
    } else {
      if (toI32(keep.length) < left) {
        this.pool.give(keep)
        keep = this.pool.take(left)
      }
      copyBytes(keep, 0, buf, at, left)
      conns.pend[c] = keep
    }
    conns.set(c, C_PEND, left)
  }

  /** The socket of `c` has room again: sends what it kept. */
  writable(c: i32): void {
    const conns: Conns = this.conns
    const off: i32 = conns.get(c, C_OUT_OFF)
    const len: i32 = conns.get(c, C_OUT_LEN)
    if (len > off) {
      const n: i32 = netWrite(conns.get(c, C_FD), conns.out[c], off, len - off)
      if (n < 0 && n !== -11) {
        this.closeConn(c)
        return
      }
      if (n > 0 && off + n < len) {
        conns.set(c, C_OUT_OFF, off + n)
        return
      }
      if (n <= 0) {
        return
      }
    }
    conns.set(c, C_OUT_OFF, 0)
    conns.set(c, C_OUT_LEN, 0)
    this.pool.give(conns.out[c])
    conns.out[c] = conns.empty
    const flags: i32 = conns.get(c, C_FLAGS)
    conns.set(c, C_FLAGS, flags & ~F_WRITE)
    if ((flags & F_CLOSE) !== 0 && conns.get(c, C_REQ) < 0) {
      this.closeConn(c)
      return
    }
    pollModify(this.loop, conns.get(c, C_FD), WANT_READ, c)
  }

  /** Sends `buf[from, from + len)` on `c`, keeping what the socket does not take; false when `c` failed and was closed. */
  send(c: i32, buf: u8[], from: i32, len: i32): boolean {
    const conns: Conns = this.conns
    const queued: i32 = conns.get(c, C_OUT_LEN)
    let sent: i32 = 0
    if (queued === 0) {
      const n: i32 = netWrite(conns.get(c, C_FD), buf, from, len)
      if (n < 0 && n !== -11) {
        this.closeConn(c)
        return false
      }
      sent = n > 0 ? n : 0
      if (sent === len) {
        return true
      }
    }
    // The rest waits for room, behind anything already waiting.
    const rest: i32 = len - sent
    let q: u8[] = conns.out[c]
    const off: i32 = conns.get(c, C_OUT_OFF)
    const have: i32 = queued - off
    if (toI32(q.length) < have + rest || off > 0) {
      const next: u8[] = this.pool.take(have + rest)
      if (have > 0) {
        copyBytes(next, 0, q, off, have)
      }
      this.pool.give(q)
      q = next
      conns.out[c] = q
    }
    copyBytes(q, have, buf, from + sent, rest)
    conns.set(c, C_OUT_OFF, 0)
    conns.set(c, C_OUT_LEN, have + rest)
    const flags: i32 = conns.get(c, C_FLAGS)
    if ((flags & F_WRITE) === 0) {
      conns.set(c, C_FLAGS, flags | F_WRITE)
      pollModify(this.loop, conns.get(c, C_FD), WANT_READ | WANT_WRITE, c)
    }
    return true
  }

  /** A head this server refuses: 400, and the connection closes. */
  refuse(c: i32): void {
    const conns: Conns = this.conns
    conns.set(c, C_FLAGS, conns.get(c, C_FLAGS) | F_CLOSE)
    const out: Out = this.errOut
    out.open()
    out.bytes(this.badRequest, 0, toI32(this.badRequest.length))
    const at: i32 = this.heads.write(out.buf, out.start, 400, out.end - out.start, this.date.current(), true)
    if (this.send(c, out.buf, at, out.end - at)) {
      this.closeIfDone(c)
    }
  }

  /** Closes `c` if it asked to close and has nothing left to send. */
  closeIfDone(c: i32): void {
    const conns: Conns = this.conns
    if (conns.get(c, C_FD) >= 0 && (conns.get(c, C_FLAGS) & F_CLOSE) !== 0 && conns.get(c, C_OUT_LEN) === 0 && conns.get(c, C_REQ) < 0) {
      this.closeConn(c)
    }
  }

  closeConn(c: i32): void {
    const conns: Conns = this.conns
    const fd: i32 = conns.get(c, C_FD)
    if (fd < 0) {
      return
    }
    netClose(fd)
    conns.set(c, C_FD, -1)
    conns.set(c, C_GEN, conns.get(c, C_GEN) + 1)
    conns.set(c, C_PEND, 0)
    conns.set(c, C_OUT_OFF, 0)
    conns.set(c, C_OUT_LEN, 0)
    conns.set(c, C_FLAGS, 0)
    this.pool.give(conns.pend[c])
    this.pool.give(conns.out[c])
    conns.pend[c] = conns.empty
    conns.out[c] = conns.empty
    const r: i32 = conns.get(c, C_REQ)
    conns.set(c, C_REQ, -1)
    // A write waiting to commit is freed when its group has.
    if (r >= 0 && this.reqs[r].status === 0) {
      this.release(this.reqs[r])
    }
    conns.free.push(c)
  }

  take(c: i32): Req {
    if (this.free.length === 0) {
      this.reqs.push(new Req(toI32(this.reqs.length)))
      this.free.push(toI32(this.reqs.length) - 1)
    }
    const r: Req = this.reqs[this.free[this.free.length - 1]]
    this.free.pop()
    r.slot = c
    r.gen = this.conns.get(c, C_GEN)
    r.route = R_NONE
    r.error = -1
    r.postId = 0
    r.userId = 0
    r.nameLength = 0
    r.bodyLength = 0
    r.status = 0
    this.conns.set(c, C_REQ, r.index)
    return r
  }

  release(r: Req): void {
    if (r.slot >= 0 && this.conns.get(r.slot, C_REQ) === r.index) {
      this.conns.set(r.slot, C_REQ, -1)
    }
    r.slot = -1
    r.status = 0
    this.free.push(r.index)
  }

  // ---- Answers ----------------------------------------------------------------------

  /**
   * Answers `r` with `status` and the body `buf[start, start + len)`, if its
   * connection is still the one it came on, and frees `r`. The head is
   * written into the `HEAD_ROOM` bytes before `start`, so head and body
   * leave in one write.
   */
  answer(r: Req, status: i32, buf: u8[], start: i32, len: i32): void {
    const c: i32 = r.slot
    const conns: Conns = this.conns
    const live: boolean = c >= 0 && conns.get(c, C_FD) >= 0 && conns.get(c, C_GEN) === r.gen
    this.release(r)
    if (!live) {
      return
    }
    const close: boolean = (conns.get(c, C_FLAGS) & F_CLOSE) !== 0
    const at: i32 = this.heads.write(buf, start, status, len, this.date.current(), close)
    if (this.send(c, buf, at, start + len - at)) {
      this.closeIfDone(c)
    }
  }

  fail(r: Req, error: i32): void {
    const body: u8[] = this.errorBodies[error]
    const out: Out = this.errOut
    out.open()
    out.bytes(body, 0, toI32(body.length))
    this.answer(r, this.errorStatus[error], out.buf, out.start, out.end - out.start)
  }

  // ---- A request ------------------------------------------------------------------

  /** Serves the request whose head `h` parsed from `b`, its body `b[bodyAt, stop)`. */
  request(c: i32, b: u8[], h: Head, bodyAt: i32, stop: i32): void {
    const r: Req = this.take(c)
    const get: boolean = h.method === M_GET
    const post: boolean = h.method === M_POST
    let end: i32 = h.targetEnd
    for (let k: i32 = h.targetStart; k < h.targetEnd; k++) {
      if (toI32(b[k]) === 63) {
        end = k
        break
      }
    }
    const start: i32 = h.targetStart
    if (bytesAre(b, start, end, "/feed")) {
      r.route = get ? R_FEED : R_NONE
    } else if (bytesAre(b, start, end, "/posts")) {
      r.route = post ? R_CREATE : R_NONE
    } else if (end - start > 7 && bytesAre(b, start, start + 7, "/posts/")) {
      let idEnd: i32 = end
      if (end - start > 12 && bytesAre(b, end - 5, end, "/like")) {
        idEnd = end - 5
        r.route = post ? R_LIKE : R_NONE
      } else {
        r.route = get ? R_POST : R_NONE
      }
      if (r.route !== R_NONE) {
        r.postId = idOf(b, start + 7, idEnd)
        // `/posts/a/b` is no route at all; an id that is not one is 400, after auth for a like.
        for (let k: i32 = start + 7; k < idEnd; k++) {
          if (toI32(b[k]) === 47) {
            r.route = R_NONE
          }
        }
      }
    } else if (bytesAre(b, start, end, "/health")) {
      r.route = get ? R_HEALTH : R_NONE
    }
    if (r.route === R_NONE) {
      r.error = E_NOT_FOUND
    } else {
      if (r.route === R_CREATE || r.route === R_LIKE) {
        r.error = this.auth(r, b, h)
      }
      if (r.error < 0 && (r.route === R_POST || r.route === R_LIKE) && r.postId < 0) {
        r.error = E_BAD_ID
      }
    }
    if (r.error < 0 && r.route === R_CREATE && stop > bodyAt) {
      r.addBody(b, bodyAt, stop - bodyAt)
    }
    this.onEnd(r)
  }

  /** Reads `r`'s bearer token; answers -1 when it is good, else the error. */
  auth(r: Req, b: u8[], h: Head): i32 {
    const start: i32 = h.authStart
    const end: i32 = h.authEnd
    if (start < 0 || end - start < 7 || !bytesAre(b, start, start + 7, "Bearer ")) {
      return E_NO_TOKEN
    }
    const now: i64 = this.passMs / 1000
    const v: i32 = this.jwt.verify(b, start + 7, end - start - 7, now)
    if (v !== JWT_OK) {
      return v === JWT_BAD_PAYLOAD ? E_BAD_PAYLOAD : E_BAD_TOKEN
    }
    r.userId = this.jwt.sub
    const n: i32 = this.jwt.nameLength
    if (n > toI32(r.name.length)) {
      r.name = new Array<u8>(n)
    }
    for (let k: i32 = 0; k < n; k++) {
      r.name[k] = this.jwt.name[k]
    }
    r.nameLength = n
    return -1
  }

  onEnd(r: Req): void {
    if (r.error >= 0) {
      this.fail(r, r.error)
      return
    }
    if (r.route === R_FEED) {
      this.startRead()
      this.readFeed(r)
    } else if (r.route === R_POST) {
      this.startRead()
      this.readPost(r)
    } else if (r.route === R_HEALTH) {
      this.health(r)
    } else if (r.route === R_CREATE) {
      const v: i32 = this.bodies.read(r.body, 0, r.bodyLength)
      if (v !== BODY_OK) {
        this.fail(r, v === BODY_MALFORMED ? E_MALFORMED : v === BODY_REQUIRED ? E_REQUIRED : E_TOO_LONG)
        return
      }
      // The trimmed post replaces the request body, for the commit.
      const s: i32 = this.bodies.start
      const n: i32 = this.bodies.end - s
      for (let k: i32 = 0; k < n; k++) {
        r.body[k] = this.bodies.text[s + k]
      }
      r.bodyLength = n
      r.status = 1
      this.writes.push(r.index)
    } else {
      r.status = 1
      this.writes.push(r.index)
    }
  }

  // ---- Reads ------------------------------------------------------------------------

  /**
   * Opens the pass's read transaction, if it is not open: every read of the
   * pass then shares one snapshot and one WAL read lock, taken and dropped
   * once a pass rather than once a statement. Nothing is kept past the pass.
   */
  startRead(): void {
    if (!this.reading) {
      this.reading = this.db.beginRead().isOk()
    }
  }

  /** Ends the pass's read transaction, which closes its cursors. */
  endRead(): void {
    const r = this.db.endRead()
    this.reading = false
    this.cursorsOpen = false
    if (r.isErr()) {
      return
    }
  }

  /**
   * Opens the pass's four cursors, if they are not open; false when they
   * cannot be (cursors are off, there is no read transaction, or the schema
   * is not the one they read), and the caller answers through SQL. Each
   * `openCursor` checks the table's or index's columns against
   * `sqlite_schema`, so a different schema is refused, never misread.
   */
  openCursors(): boolean {
    if (this.cursorsOpen) {
      return true
    }
    if (!this.useCursors || !this.reading) {
      return false
    }
    // `openCursor` checks the columns against `sqlite_schema`, so the
    // records `postHeadCursor` decodes hold them in this order. The four
    // `Cursor` objects are made once and reopened by every later read:
    // there is no garbage collector, and four new ones a pass stayed
    // allocated for good.
    const a = this.newestCur
    const b = this.postCur
    const c = this.userCur
    const d = this.likeCur
    if (a !== null && b !== null && c !== null && d !== null) {
      this.cursorsOpen =
        this.db.reopenCursor(a, "posts_created_at_id_idx", "created_at DESC,id DESC,rowid").isOk() &&
        this.db.reopenCursor(b, "posts", "id,user_id,body,created_at").isOk() &&
        this.db.reopenCursor(c, "users", "id,username,created_at").isOk() &&
        this.db.reopenCursor(d, "likes_post_id_idx", "post_id,rowid").isOk()
      return this.cursorsOpen
    }
    const na = this.db.openCursor("posts_created_at_id_idx", "created_at DESC,id DESC,rowid")
    const nb = this.db.openCursor("posts", "id,user_id,body,created_at")
    const nc = this.db.openCursor("users", "id,username,created_at")
    const nd = this.db.openCursor("likes_post_id_idx", "post_id,rowid")
    if (na.isErr() || nb.isErr() || nc.isErr() || nd.isErr()) {
      return false
    }
    this.newestCur = na.value
    this.postCur = nb.value
    this.userCur = nc.value
    this.likeCur = nd.value
    this.cursorsOpen = true
    return true
  }

  /**
   * Post `id`'s object up to and including `"like_count":`, into `out`, read
   * through the open cursors: its row by rowid, read as one record
   * (`Cursor.record`), then its author's. 1 when written, 0 when the post or its author is not there
   * (the reference JOIN drops a post without its user), -1 when SQLite failed.
   */
  postHeadCursor(id: i64, step: boolean, out: Out): i32 {
    const posts = this.postCur
    const users = this.userCur
    if (posts === null || users === null) {
      return -1
    }
    // `step`: the post is likely the row before the one just read.
    let found: boolean = false
    if (step) {
      const q = posts.previous()
      if (q.isErr()) {
        return -1
      }
      found = q.value && posts.rowid() === id
    }
    if (!found) {
      const p = posts.seekRowid(id)
      if (p.isErr()) {
        return -1
      }
      if (!p.value) {
        return 0
      }
    }
    // The post's record: its id (NULL, the rowid), user_id, body, created_at.
    const pr: RowRecord = this.postRec
    if (pr.read(posts).isErr()) {
      return -1
    }
    // A user_id that is not an integer finds no author here: the reference
    // query, which compares it as SQL does, answers instead.
    if (pr.type(1) !== SQLITE_INTEGER) {
      return 0
    }
    const u = users.seekRowid(pr.int(1))
    if (u.isErr()) {
      return -1
    }
    if (!u.value) {
      return 0
    }
    const ur: RowRecord = this.userRec
    if (ur.read(users).isErr() || pr.type(2) !== SQLITE_TEXT || pr.type(3) !== SQLITE_TEXT || ur.type(1) !== SQLITE_TEXT) {
      return -1
    }
    out.bytes(this.frag[0], 0, 6)
    out.int(id)
    out.bytes(this.frag[1], 0, 8)
    out.string(pr.buf, pr.start(2), pr.size(2))
    out.bytes(this.frag[2], 0, 14)
    out.string(pr.buf, pr.start(3), pr.size(3))
    out.bytes(this.frag[3], 0, 10)
    out.string(ur.buf, ur.start(1), ur.size(1))
    out.bytes(this.frag[4], 0, 14)
    return 1
  }

  /** Post `id`'s entries in `likes_post_id_idx`, by one `countRange`; -1 when SQLite failed. */
  countLikes(id: i64): i64 {
    const likes = this.likeCur
    if (likes === null) {
      return -1
    }
    const n = likes.countRange(id, id, this.likeSpan)
    return n.isErr() ? -1 : toI64(n.value)
  }

  /**
   * Post `id`'s whole object into `out`, read through the open cursors. 1 when
   * written, 0 when the post or its author is not there, -1 when SQLite failed.
   */
  cursorPost(id: i64, out: Out): i32 {
    const w: i32 = this.postHeadCursor(id, false, out)
    if (w <= 0) {
      return w
    }
    const count: i64 = this.countLikes(id)
    if (count < 0) {
      return -1
    }
    out.int(count)
    out.byte(125)
    return 1
  }


  /**
   * `GET /feed` through cursors. The 20 newest ids come from
   * `posts_created_at_id_idx`, from its first entry: its order is exactly
   * `(created_at DESC, id DESC)`. Their like counts come first, from one walk
   * of `likes_post_id_idx` when the ids span less than `RANGE_MAX`, else a
   * seek each. Then each post is read in the feed's order and written
   * straight into the answer: a post whose id is one below the last one's is
   * the cursor's previous row, on the leaf the last read left (the newest
   * posts nearly always are), any other a seek. A post or author that is
   * missing, which the reference query's JOIN would drop, hands the request
   * to the reference SQL. False, having written nothing, when the cursors
   * cannot be opened.
   */
  readFeedCursors(r: Req): boolean {
    if (!this.openCursors()) {
      return false
    }
    const ix = this.newestCur
    if (ix === null) {
      return false
    }
    const ids: i64[] = this.feedIds
    let n: i32 = 0
    const first = ix.first()
    let ok: boolean = true
    let on: boolean = false
    if (first.isOk()) {
      on = first.value
    } else {
      ok = false
    }
    while (ok && on && n < 20) {
      ids[n] = ix.rowid()
      n++
      if (n < 20) {
        const s = ix.next()
        if (s.isOk()) {
          on = s.value
        } else {
          ok = false
        }
      }
    }
    if (ok && n > 0) {
      let lo: i64 = ids[0]
      let hi: i64 = ids[0]
      for (let k: i32 = 1; k < n; k++) {
        if (ids[k] < lo) {
          lo = ids[k]
        }
        if (ids[k] > hi) {
          hi = ids[k]
        }
      }
      const likes = this.likeCur
      if (likes !== null && hi - lo < toI64(RANGE_MAX)) {
        // One seek and one walk of likes_post_id_idx for all of them.
        const span: i64[] = this.likeSpan
        ok = likes.countRange(lo, hi, span).isOk()
        for (let k: i32 = 0; k < n && ok; k++) {
          this.likes[k] = span[toI32(ids[k] - lo)]
        }
      } else {
        for (let k: i32 = 0; k < n && ok; k++) {
          const c: i64 = this.countLikes(ids[k])
          if (c < 0) {
            ok = false
          } else {
            this.likes[k] = c
          }
        }
      }
    }
    if (!ok) {
      this.fail(r, E_INTERNAL)
      return true
    }
    const out: Out = this.out
    out.open()
    out.bytes(this.feedOpen, 0, 10)
    for (let k: i32 = 0; k < n; k++) {
      if (k > 0) {
        out.byte(44)
      }
      const w: i32 = this.postHeadCursor(ids[k], k > 0 && ids[k] === ids[k - 1] - 1, out)
      if (w < 0) {
        this.fail(r, E_INTERNAL)
        return true
      }
      if (w === 0) {
        this.readFeedReference(r)
        return true
      }
      out.int(this.likes[k])
      out.byte(125)
    }
    out.byte(93)
    out.byte(125)
    if (out.failed) {
      this.fail(r, E_INTERNAL)
      return true
    }
    this.answer(r, 200, out.buf, out.start, out.end - out.start)
    return true
  }


  /** Appends the text column `c` of `st`'s row to `out` as a JSON string; false when SQLite failed. */
  textColumn(st: Statement, c: i32, out: Out): boolean {
    // Each Result is a `const`: one reassigned is boxed in the arena, and
    // the loop's arena keeps it.
    const r = st.columnInto(c, this.col, 0)
    if (r.isErr()) {
      return false
    }
    if (r.value <= toI32(this.col.length)) {
      out.string(this.col, 0, r.value)
      return true
    }
    this.col = new Array<u8>(r.value)
    const again = st.columnInto(c, this.col, 0)
    if (again.isErr()) {
      return false
    }
    out.string(this.col, 0, again.value)
    return true
  }

  /**
   * The post object of `st`'s current row (id, body, created_at, username in
   * columns 0 to 3) into `out`, up to and including `"like_count":`.
   */
  postHead(st: Statement, out: Out): boolean {
    out.bytes(this.frag[0], 0, 6)
    out.int(st.columnI64(0))
    out.bytes(this.frag[1], 0, 8)
    if (!this.textColumn(st, 1, out)) {
      return false
    }
    out.bytes(this.frag[2], 0, 14)
    if (!this.textColumn(st, 2, out)) {
      return false
    }
    out.bytes(this.frag[3], 0, 10)
    if (!this.textColumn(st, 3, out)) {
      return false
    }
    out.bytes(this.frag[4], 0, 14)
    return true
  }

  /** The whole post object of `st`'s current row, its like count in column 4. */
  postObject(st: Statement, out: Out): boolean {
    if (!this.postHead(st, out)) {
      return false
    }
    out.int(st.columnI64(4))
    out.byte(125)
    return true
  }

  /**
   * `GET /feed`. The 20 newest ids come from the `(created_at, id)` index
   * alone. When they lie within `RANGE_MAX` of each other, as the newest
   * posts nearly always do, their rows (with their authors) are one range
   * scan of `posts` in id order, and their like counts one range scan of
   * `likes_post_id_idx`, instead of 20 seeks of each; rows in the range
   * that are not among the 20 are skipped. Otherwise, or if any of the 20 is
   * not found in the range (a post without its user, which the reference
   * query's JOIN would drop), the reference query answers. All three
   * statements read the pass's one snapshot.
   */
  readFeed(r: Req): void {
    // Through cursors when they open; the SQL range reads below otherwise.
    if (this.readFeedCursors(r)) {
      return
    }
    const ids: i64[] = this.feedIds
    let n: i32 = 0
    const st: Statement = this.newest
    let ok: boolean = true
    while (n < 20) {
      const s = st.step()
      if (s.isErr()) {
        ok = false
        break
      }
      if (!s.value) {
        break
      }
      ids[n] = st.columnI64(0)
      n++
    }
    settle(st)
    if (!ok) {
      this.fail(r, E_INTERNAL)
      return
    }
    let lo: i64 = 0
    let hi: i64 = 0
    for (let k: i32 = 0; k < n; k++) {
      if (k === 0 || ids[k] < lo) {
        lo = ids[k]
      }
      if (k === 0 || ids[k] > hi) {
        hi = ids[k]
      }
    }
    if (n === 0 || hi - lo >= toI64(RANGE_MAX)) {
      this.readFeedReference(r)
      return
    }
    const at: i32[] = this.rangeAt
    for (let k: i32 = 0; k < n; k++) {
      at[toI32(ids[k] - lo)] = k
    }
    const rows: Out = this.rows
    rows.reset()
    let found: i32 = 0
    const range: Statement = this.postRange
    ok = range.bindI64(1, lo).isOk() && range.bindI64(2, hi).isOk()
    while (ok) {
      const s = range.step()
      if (s.isErr()) {
        ok = false
      } else if (!s.value) {
        break
      } else {
        const id: i64 = range.columnI64(0)
        const k: i32 = id >= lo && id <= hi ? at[toI32(id - lo)] : -1
        if (k >= 0) {
          this.rowStart[k] = rows.end
          ok = this.postHead(range, rows)
          this.rowEnd[k] = rows.end
          this.likes[k] = 0
          found++
        }
      }
    }
    settle(range)
    const counts: Statement = this.likeRange
    ok = ok && counts.bindI64(1, lo).isOk() && counts.bindI64(2, hi).isOk()
    while (ok) {
      const s = counts.step()
      if (s.isErr()) {
        ok = false
      } else if (!s.value) {
        break
      } else {
        const id: i64 = counts.columnI64(0)
        const k: i32 = id >= lo && id <= hi ? at[toI32(id - lo)] : -1
        if (k >= 0) {
          this.likes[k] = counts.columnI64(1)
        }
      }
    }
    settle(counts)
    for (let k: i32 = 0; k < n; k++) {
      at[toI32(ids[k] - lo)] = -1
    }
    if (!ok || rows.failed) {
      this.fail(r, E_INTERNAL)
      return
    }
    if (found !== n) {
      this.readFeedReference(r)
      return
    }
    const out: Out = this.out
    out.open()
    out.bytes(this.feedOpen, 0, 10)
    for (let k: i32 = 0; k < n; k++) {
      if (k > 0) {
        out.byte(44)
      }
      out.bytes(rows.buf, this.rowStart[k], this.rowEnd[k] - this.rowStart[k])
      out.int(this.likes[k])
      out.byte(125)
    }
    out.byte(93)
    out.byte(125)
    if (out.failed) {
      this.fail(r, E_INTERNAL)
      return
    }
    this.answer(r, 200, out.buf, out.start, out.end - out.start)
  }

  /** The feed by SPEC.md's reference query, one statement: what `readFeed` falls back to. */
  readFeedReference(r: Req): void {
    const st: Statement = this.feed
    const out: Out = this.out
    out.open()
    out.bytes(this.feedOpen, 0, 10)
    let rows: i32 = 0
    let ok: boolean = true
    while (ok) {
      const s = st.step()
      if (s.isErr()) {
        ok = false
      } else if (!s.value) {
        break
      } else {
        if (rows > 0) {
          out.byte(44)
        }
        ok = this.postObject(st, out)
        rows++
      }
    }
    settle(st)
    if (!ok || out.failed) {
      this.fail(r, E_INTERNAL)
      return
    }
    out.byte(93)
    out.byte(125)
    this.answer(r, 200, out.buf, out.start, out.end - out.start)
  }

  readPost(r: Req): void {
    const out: Out = this.out
    out.open()
    if (this.openCursors()) {
      out.bytes(this.postOpen, 0, 8)
      const w: i32 = this.cursorPost(r.postId, out)
      if (w < 0 || out.failed) {
        this.fail(r, E_INTERNAL)
      } else if (w === 0) {
        this.fail(r, E_NO_POST)
      } else {
        out.byte(125)
        this.answer(r, 200, out.buf, out.start, out.end - out.start)
      }
      return
    }
    const st: Statement = this.post
    if (st.bindI64(1, r.postId).isErr()) {
      this.fail(r, E_INTERNAL)
      return
    }
    const s = st.step()
    if (s.isErr()) {
      settle(st)
      this.fail(r, E_INTERNAL)
      return
    }
    if (!s.value) {
      settle(st)
      this.fail(r, E_NO_POST)
      return
    }
    out.bytes(this.postOpen, 0, 8)
    const ok: boolean = this.postObject(st, out)
    settle(st)
    if (!ok || out.failed) {
      this.fail(r, E_INTERNAL)
      return
    }
    out.byte(125)
    this.answer(r, 200, out.buf, out.start, out.end - out.start)
  }

  health(r: Req): void {
    const s = this.ping.step()
    settle(this.ping)
    const code: i32 = s.isErr() ? s.error : 0
    const out: Out = this.out
    out.open()
    if (s.isErr() || !s.value) {
      out.bytes(this.degraded, 0, 48)
      const msg: u8[] = bytesOf(this.db.error(code).message)
      out.string(msg, 0, toI32(msg.length))
      out.byte(125)
      this.answer(r, 503, out.buf, out.start, out.end - out.start)
      return
    }
    out.bytes(this.healthy, 0, 36)
    out.int((monotonicNanos() - this.startedNs) / 1000000000)
    out.byte(125)
    this.answer(r, 200, out.buf, out.start, out.end - out.start)
  }

  // ---- Writes -----------------------------------------------------------------------

  /**
   * Runs the pass's writes in one transaction and answers them once it has
   * committed. When another process holds the write lock (another server's
   * group, or the checkpointer's RESTART), `BEGIN IMMEDIATE` is busy at once
   * (busy_timeout=0): the writes stay queued and the next pass, at most a
   * millisecond later, tries again, while reads go on being answered.
   */
  commitWrites(): void {
    const b = this.begin.step()
    settle(this.begin)
    let ok: boolean = true
    if (b.isErr()) {
      if ((b.error & 255) === SQLITE_BUSY) {
        this.held = true
        return
      }
      ok = false
    }
    this.held = false
    // The batch is swapped out first: answering a slot drives it, and a
    // pipelined write behind the answer joins the next group.
    const batch: i32[] = this.writes
    this.writes = this.spare
    this.spare = batch
    if (ok) {
      for (let k: i32 = 0; k < toI32(batch.length) && ok; k++) {
        const r: Req = this.reqs[batch[k]]
        ok = r.route === R_CREATE ? this.createPost(r) : this.like(r)
      }
      if (ok) {
        ok = this.commit.step().isOk()
        settle(this.commit)
      }
      if (!ok) {
        settle(this.rollback, this.rollback.step().isOk())
      }
    }
    for (let k: i32 = 0; k < toI32(batch.length); k++) {
      const r: Req = this.reqs[batch[k]]
      const slot: i32 = r.slot
      const live: boolean = slot >= 0 && this.conns.get(slot, C_FD) >= 0 && this.conns.get(slot, C_GEN) === r.gen
      if (ok && (r.status === 200 || r.status === 201)) {
        this.answer(r, r.status, r.out.buf, r.out.start, r.out.end - r.out.start)
      } else {
        this.fail(r, ok && r.status === 404 ? E_NO_POST : E_INTERNAL)
      }
      if (live && this.conns.get(slot, C_FD) >= 0 && this.conns.get(slot, C_PEND) > 0) {
        // The connection kept the requests behind this one; they are served now.
        this.serve(slot, this.conns.pend[slot], this.conns.get(slot, C_PEND))
      }
    }
    while (batch.length > 0) {
      batch.pop()
    }
  }

  /** Inserts `r`'s post and writes its answer into `r.out`; false when SQLite failed. */
  createPost(r: Req): boolean {
    const st: Statement = this.insertPost
    if (st.bindI64(1, r.userId).isErr() || st.bindTextBytes(2, r.body, 0, r.bodyLength).isErr()) {
      settle(st)
      return false
    }
    const s = st.step()
    if (s.isErr() || !s.value) {
      settle(st)
      return false
    }
    const out: Out = r.out
    out.open()
    out.bytes(this.createdOpen, 0, 14)
    out.int(st.columnI64(0))
    out.bytes(this.frag[1], 0, 8)
    out.string(r.body, 0, r.bodyLength)
    out.bytes(this.frag[2], 0, 14)
    const ok: boolean = this.textColumn(st, 1, out)
    // Stepped to its end, so the statement has finished before the commit.
    const done = st.step()
    settle(st)
    if (!ok || done.isErr()) {
      return false
    }
    out.bytes(this.frag[3], 0, 10)
    out.string(r.name, 0, r.nameLength)
    out.bytes(this.createdClose, 0, 17)
    r.status = 201
    return !out.failed
  }

  /**
   * Likes `r`'s post as its user and writes the answer into `r.out`; false
   * when SQLite failed. The post is looked up first, inside the group's
   * write transaction, and the like is then a one-row `INSERT … VALUES`: an
   * `INSERT … SELECT` is a statement that may write many rows, and SQLite
   * gives every such statement a statement journal, which inside a group's
   * transaction copied each page the group had already changed, about
   * sixteen 4 KiB pages a like.
   */
  like(r: Req): boolean {
    const e: Statement = this.postExists
    if (e.bindI64(1, r.postId).isErr()) {
      settle(e)
      return false
    }
    const found = e.step()
    settle(e)
    if (found.isErr()) {
      return false
    }
    if (!found.value) {
      r.status = 404
      return true
    }
    const st: Statement = this.insertLike
    if (st.bindI64(1, r.userId).isErr() || st.bindI64(2, r.postId).isErr()) {
      settle(st)
      return false
    }
    const s = st.step()
    settle(st)
    if (s.isErr()) {
      return false
    }
    // Nothing inserted: the user has liked the post already.
    const status: i32 = this.db.changes() === 0 ? 200 : 201
    const out: Out = r.out
    out.open()
    out.bytes(this.likedOpen, 0, 30)
    if (status === 200) {
      out.bytes(this.trueBytes, 0, 4)
    } else {
      out.bytes(this.falseBytes, 0, 5)
    }
    out.bytes(this.postIdKey, 0, 11)
    out.int(r.postId)
    out.byte(125)
    r.status = status
    return !out.failed
  }
}
