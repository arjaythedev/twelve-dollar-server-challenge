// One serving process: a single-threaded `pollWait` loop over its listener
// and its keep-alive connections (`nish/net/http1-server`), and one SQLite
// connection (`nish/sqlite`) that every request reads and writes inline.
//
// A pass of the loop is everything one `pollWait` hands back. Reads are
// answered as their request ends, each statement reading SQLite while that
// request is served. Writes wait for the end of the pass and then go as one
// group: one `BEGIN IMMEDIATE`, each write, one `COMMIT` (WAL,
// synchronous=NORMAL). Each write's response is sent only after that
// `COMMIT` returns (rule 6); if it fails, every write in the group answers
// 500 and none is retried, since none committed.
import { pollAdd, pollCreate, pollModify, pollWait } from "nish:net"
import { monotonicNanos } from "nish:process"
import {
  H1_BODY,
  H1_DONE,
  H1_END,
  H1_ERROR,
  H1_NEED_MORE,
  H1_POOL_FULL,
  H1_REQUEST,
  H1_WANT_READ,
  H1_WANT_WRITE,
  Http1Config,
  Http1Connection,
  Http1Server,
} from "nish/net/http1-server"
import { Http1Parser } from "nish/net/http1-codec"
import { Cursor, Database, SEEK_GE, SQLITE_BUSY, Statement } from "nish/sqlite"
import { BODY_MALFORMED, BODY_OK, BODY_REQUIRED, BodyReader } from "./body"
import { JWT_BAD_PAYLOAD, JWT_OK, Jwt } from "./jwt"
import { Out, bytesOf } from "./out"

const R_NONE: i32 = 0
const R_HEALTH: i32 = 1
const R_FEED: i32 = 2
const R_POST: i32 = 3
const R_CREATE: i32 = 4
const R_LIKE: i32 = 5

/** `pollWait`'s token for the listener; a connection's is its slot. */
const TOKEN_LISTENER: i32 = -1
const READABLE: i32 = 5

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
  idleTimeoutMs: i32 = 600000
  cacheKib: i32 = 65536
  mmapBytes: i64 = 0
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
  server: Http1Server
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
  rangeAt: i32[]
  rowStart: i32[]
  rowEnd: i32[]
  likes: i64[]
  rows: Out
  /** Whether the pass's read transaction is open. */
  reading: boolean = false
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
  rollback: Statement
  jwt: Jwt
  bodies: BodyReader
  reqs: Req[]
  free: i32[]
  /** The request each slot is in, or -1; and the slot's generation, bumped when it closes. */
  slotReq: i32[]
  slotGen: i32[]
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
  jsonNames: u8[][]
  jsonValues: u8[][]
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
  nextExpire: i64 = 0

  constructor(cfg: ServerConfig, listener: i32, db: Database) {
    const http = new Http1Config()
    http.idleTimeout = cfg.idleTimeoutMs
    http.maxBody = 1048576
    http.outputSize = 65536
    // Nearly every connection sits between requests: it holds no input buffer while it does.
    http.idleInput = true
    this.server = new Http1Server(http, listener, cfg.slots)
    this.loop = pollCreate()
    pollAdd(this.loop, listener, H1_WANT_READ, TOKEN_LISTENER)
    this.db = db
    this.useCursors = cfg.cursors
    this.feed = db.prepare(`${POST_SQL} ORDER BY p.created_at DESC, p.id DESC LIMIT 20`).expect("feed")
    this.post = db.prepare(`${POST_SQL} WHERE p.id = ?1`).expect("post")
    this.insertPost = db
      .prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at")
      .expect("insert post")
    this.insertLike = db
      .prepare(
        "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING",
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
    this.slotReq = new Array<i32>(cfg.slots)
    this.slotGen = new Array<i32>(cfg.slots)
    for (let k: i32 = 0; k < cfg.slots; k++) {
      this.slotReq[k] = -1
    }
    this.writes = []
    this.spare = []
    this.out = new Out(65536)
    this.col = new Array<u8>(4096)
    this.jsonNames = [bytesOf("Content-Type")]
    this.jsonValues = [bytesOf("application/json")]
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
  }

  // ---- The loop -------------------------------------------------------------------

  /** Serves until the process is stopped; answers 1 when the loop fails. */
  run(): i32 {
    const ready: i32[] = new Array<i32>(2 * 1024)
    while (true) {
      const n: i32 = pollWait(this.loop, ready, this.held ? 1 : 1000)
      if (n < 0 && n !== -4) {
        return 1
      }
      for (let k: i32 = 0; k < n; k++) {
        this.ready(ready[2 * k], ready[2 * k + 1])
      }
      if (this.reading) {
        this.endRead()
      }
      if (this.writes.length > 0) {
        this.commitWrites()
      }
      const now: i64 = monotonicNanos()
      if (now >= this.nextExpire) {
        this.server.expire(now)
        this.nextExpire = now + 1000000000
      }
    }
  }

  ready(token: i32, events: i32): void {
    if (token >= 0) {
      if (this.server.holds(token)) {
        if ((events & READABLE) !== 0) {
          this.server.readable(token)
        } else {
          this.server.writable(token)
        }
        this.drive(token)
      }
    } else if (token === TOKEN_LISTENER) {
      this.acceptAll()
    }
  }

  acceptAll(): void {
    while (true) {
      const slot: i32 = this.server.accept()
      if (slot >= 0) {
        const old: i32 = this.slotReq[slot]
        // `expire` closes a slot without telling the loop: a request it left
        // that is not waiting to commit is freed here.
        if (old >= 0 && this.reqs[old].status === 0) {
          this.release(this.reqs[old])
        }
        this.slotGen[slot] = this.slotGen[slot] + 1
        this.slotReq[slot] = -1
        pollAdd(this.loop, this.server.fd(slot), H1_WANT_READ, slot)
      } else if (slot !== H1_POOL_FULL) {
        return
      }
    }
  }

  /** Runs `slot`'s events, then re-arms or closes it. */
  drive(slot: i32): void {
    let event: i32 = this.server.next(slot)
    while (event !== H1_NEED_MORE && event !== H1_ERROR) {
      if (event === H1_REQUEST) {
        this.onHead(slot)
      } else if (event === H1_BODY) {
        const r: i32 = this.slotReq[slot]
        if (r >= 0) {
          const conn: Http1Connection = this.server.connection(slot)
          this.reqs[r].addBody(conn.data, conn.dataStart, conn.dataLength)
        }
      } else if (event === H1_END) {
        const r: i32 = this.slotReq[slot]
        if (r >= 0) {
          this.onEnd(this.reqs[r])
        }
      }
      event = this.server.next(slot)
    }
    const wants: i32 = this.server.flush(slot)
    if ((wants & H1_DONE) !== 0) {
      this.closeSlot(slot)
    } else if (this.server.rearm(slot, wants)) {
      pollModify(this.loop, this.server.fd(slot), wants & (H1_WANT_READ | H1_WANT_WRITE), slot)
    }
  }

  closeSlot(slot: i32): void {
    this.server.close(slot)
    this.slotGen[slot] = this.slotGen[slot] + 1
    const r: i32 = this.slotReq[slot]
    this.slotReq[slot] = -1
    // A write waiting to commit is freed when its group has.
    if (r >= 0 && this.reqs[r].status === 0) {
      this.release(this.reqs[r])
    }
  }

  take(slot: i32): Req {
    if (this.free.length === 0) {
      this.reqs.push(new Req(toI32(this.reqs.length)))
      this.free.push(toI32(this.reqs.length) - 1)
    }
    const r: Req = this.reqs[this.free[this.free.length - 1]]
    this.free.pop()
    r.slot = slot
    r.gen = this.slotGen[slot]
    r.route = R_NONE
    r.error = -1
    r.postId = 0
    r.userId = 0
    r.nameLength = 0
    r.bodyLength = 0
    r.status = 0
    this.slotReq[slot] = r.index
    return r
  }

  release(r: Req): void {
    if (r.slot >= 0 && this.slotReq[r.slot] === r.index) {
      this.slotReq[r.slot] = -1
    }
    r.slot = -1
    r.status = 0
    this.free.push(r.index)
  }

  // ---- Answers ----------------------------------------------------------------------

  /** Sends `buf[0, len)` with `status` on `r`'s slot, if it is still the connection `r` came on, and frees `r`. */
  answer(r: Req, status: i32, buf: u8[], len: i32): void {
    const slot: i32 = r.slot
    const live: boolean = slot >= 0 && this.server.holds(slot) && this.slotGen[slot] === r.gen
    this.release(r)
    if (!live) {
      return
    }
    const conn: Http1Connection = this.server.connection(slot)
    conn.respond(status, this.jsonNames, this.jsonValues, len)
    conn.sendBody(buf, 0, len)
    conn.end()
  }

  fail(r: Req, error: i32): void {
    const body: u8[] = this.errorBodies[error]
    this.answer(r, this.errorStatus[error], body, toI32(body.length))
  }

  // ---- A request ------------------------------------------------------------------

  onHead(slot: i32): void {
    const r: Req = this.take(slot)
    const p: Http1Parser = this.server.connection(slot).parser
    const b: u8[] = p.head
    const get: boolean = bytesAre(b, p.methodStart, p.methodEnd, "GET")
    const post: boolean = !get && bytesAre(b, p.methodStart, p.methodEnd, "POST")
    let end: i32 = p.targetEnd
    for (let k: i32 = p.targetStart; k < p.targetEnd; k++) {
      if (toI32(b[k]) === 63) {
        end = k
        break
      }
    }
    const start: i32 = p.targetStart
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
      return
    }
    if (r.route === R_CREATE || r.route === R_LIKE) {
      r.error = this.auth(r, p)
      if (r.error >= 0) {
        return
      }
    }
    if ((r.route === R_POST || r.route === R_LIKE) && r.postId < 0) {
      r.error = E_BAD_ID
    }
  }

  /** Reads `r`'s bearer token; answers -1 when it is good, else the error. */
  auth(r: Req, p: Http1Parser): i32 {
    const h: i32 = p.headerIndex("authorization")
    if (h < 0) {
      return E_NO_TOKEN
    }
    const start: i32 = p.spans[4 * h + 2]
    const end: i32 = p.spans[4 * h + 3]
    if (end - start < 7 || !bytesAre(p.head, start, start + 7, "Bearer ")) {
      return E_NO_TOKEN
    }
    const now: i64 = toI64(Date.now()) / 1000
    const v: i32 = this.jwt.verify(p.head, start + 7, end - start - 7, now)
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
    this.newestCur = null
    this.postCur = null
    this.userCur = null
    this.likeCur = null
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
    if (this.likeCur !== null) {
      return true
    }
    if (!this.useCursors || !this.reading) {
      return false
    }
    const a = this.db.openCursor("posts_created_at_id_idx", "created_at DESC,id DESC,rowid")
    const b = this.db.openCursor("posts", "id,user_id,body,created_at")
    const c = this.db.openCursor("users", "id,username,created_at")
    const d = this.db.openCursor("likes_post_id_idx", "post_id,rowid")
    if (a.isErr() || b.isErr() || c.isErr() || d.isErr()) {
      return false
    }
    this.newestCur = a.value
    this.postCur = b.value
    this.userCur = c.value
    this.likeCur = d.value
    return true
  }

  /** Appends text column `c` of `cur`'s row to `out` as a JSON string; false when SQLite failed. */
  textCursor(cur: Cursor, c: i32, out: Out): boolean {
    const r = cur.columnInto(c, this.col, 0)
    if (r.isErr()) {
      return false
    }
    if (r.value <= toI32(this.col.length)) {
      out.string(this.col, 0, r.value)
      return true
    }
    this.col = new Array<u8>(r.value)
    const again = cur.columnInto(c, this.col, 0)
    if (again.isErr()) {
      return false
    }
    out.string(this.col, 0, again.value)
    return true
  }

  /**
   * Post `id`'s whole object into `out`, read through the open cursors: its
   * row by rowid, its author's by rowid, and its likes counted in
   * `likes_post_id_idx`, the reference query's three reads without its
   * bytecode. 1 when written, 0 when the post or its author is not there (the
   * reference JOIN drops a post without its user), -1 when SQLite failed.
   */
  cursorPost(id: i64, out: Out): i32 {
    const posts = this.postCur
    const users = this.userCur
    const likes = this.likeCur
    if (posts === null || users === null || likes === null) {
      return -1
    }
    const p = posts.seekRowid(id)
    if (p.isErr()) {
      return -1
    }
    if (!p.value) {
      return 0
    }
    const u = users.seekRowid(posts.columnI64(1))
    if (u.isErr()) {
      return -1
    }
    if (!u.value) {
      return 0
    }
    let count: i64 = 0
    if (likes.keyReset().isErr() || likes.keyI64(id).isErr()) {
      return -1
    }
    const first = likes.seek(SEEK_GE)
    if (first.isErr()) {
      return -1
    }
    let on: boolean = first.value
    while (on && likes.columnI64(0) === id) {
      count++
      const n = likes.next()
      if (n.isErr()) {
        return -1
      }
      on = n.value
    }
    out.bytes(this.frag[0], 0, 6)
    out.int(id)
    out.bytes(this.frag[1], 0, 8)
    if (!this.textCursor(posts, 2, out)) {
      return -1
    }
    out.bytes(this.frag[2], 0, 14)
    if (!this.textCursor(posts, 3, out)) {
      return -1
    }
    out.bytes(this.frag[3], 0, 10)
    if (!this.textCursor(users, 1, out)) {
      return -1
    }
    out.bytes(this.frag[4], 0, 14)
    out.int(count)
    out.byte(125)
    return 1
  }

  /**
   * `GET /feed` through cursors: `posts_created_at_id_idx` from its first
   * entry, newest first in `(created_at DESC, id DESC)` order, and each entry's
   * post object until 20 are written; an entry whose post or author is
   * missing is skipped, as the reference query's JOIN skips it. False, having
   * written nothing, when the cursors cannot be opened.
   */
  readFeedCursors(r: Req): boolean {
    if (!this.openCursors()) {
      return false
    }
    const ix = this.newestCur
    if (ix === null) {
      return false
    }
    const out: Out = this.out
    out.reset()
    out.bytes(this.feedOpen, 0, 10)
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
      const mark: i32 = out.end
      if (n > 0) {
        out.byte(44)
      }
      const w: i32 = this.cursorPost(ix.rowid(), out)
      if (w < 0) {
        ok = false
      } else {
        if (w === 0) {
          out.end = mark
        } else {
          n++
        }
        if (n < 20) {
          const s = ix.next()
          if (s.isOk()) {
            on = s.value
          } else {
            ok = false
          }
        }
      }
    }
    if (!ok || out.failed) {
      this.fail(r, E_INTERNAL)
      return true
    }
    out.byte(93)
    out.byte(125)
    this.answer(r, 200, out.buf, out.end)
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
    out.reset()
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
    this.answer(r, 200, out.buf, out.end)
  }

  /** The feed by SPEC.md's reference query, one statement: what `readFeed` falls back to. */
  readFeedReference(r: Req): void {
    const st: Statement = this.feed
    const out: Out = this.out
    out.reset()
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
    this.answer(r, 200, out.buf, out.end)
  }

  readPost(r: Req): void {
    const out: Out = this.out
    out.reset()
    if (this.openCursors()) {
      out.bytes(this.postOpen, 0, 8)
      const w: i32 = this.cursorPost(r.postId, out)
      if (w < 0 || out.failed) {
        this.fail(r, E_INTERNAL)
      } else if (w === 0) {
        this.fail(r, E_NO_POST)
      } else {
        out.byte(125)
        this.answer(r, 200, out.buf, out.end)
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
    this.answer(r, 200, out.buf, out.end)
  }

  health(r: Req): void {
    const s = this.ping.step()
    settle(this.ping)
    const code: i32 = s.isErr() ? s.error : 0
    const out: Out = this.out
    out.reset()
    if (s.isErr() || !s.value) {
      out.bytes(this.degraded, 0, 48)
      const msg: u8[] = bytesOf(this.db.error(code).message)
      out.string(msg, 0, toI32(msg.length))
      out.byte(125)
      this.answer(r, 503, out.buf, out.end)
      return
    }
    out.bytes(this.healthy, 0, 36)
    out.int((monotonicNanos() - this.startedNs) / 1000000000)
    out.byte(125)
    this.answer(r, 200, out.buf, out.end)
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
      const live: boolean = slot >= 0 && this.server.holds(slot) && this.slotGen[slot] === r.gen
      if (ok && (r.status === 200 || r.status === 201)) {
        this.answer(r, r.status, r.out.buf, r.out.end)
      } else {
        this.fail(r, ok && r.status === 404 ? E_NO_POST : E_INTERNAL)
      }
      if (live) {
        // The slot may hold the next pipelined request; it is read now.
        this.drive(slot)
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
    out.reset()
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

  /** Likes `r`'s post as its user and writes the answer into `r.out`; false when SQLite failed. */
  like(r: Req): boolean {
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
    let status: i32 = 201
    if (this.db.changes() === 0) {
      // Nothing inserted: a like there already, or no such post.
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
      status = 200
    }
    const out: Out = r.out
    out.reset()
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
