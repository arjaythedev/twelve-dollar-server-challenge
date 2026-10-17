## Data-access layer. Owns every SQL statement in the submission.
##
## Uses the raw bindings from `sqlite3` directly (no std/db_sqlite, no ORM).
## Statements are prepared once at open time with SQLITE_PREPARE_PERSISTENT and
## reused; each call resets and re-binds.
##
## Write batching (group commit)
## -----------------------------
## By default (`setGroupCommit` off) every write runs in autocommit mode, so it
## is durable before the proc returns. When group commit is enabled the server
## opens ONE `BEGIN IMMEDIATE` per event-loop pass (the first write of the pass
## calls `beginBatch`) and `COMMIT`s it once, at the end of the pass. Writes and
## reads executed while the batch is open all run inside that single
## transaction, so a read issued after a write in the same pass observes the
## write, and the whole pass becomes durable at one commit.
##
## IMPORTANT: no SAVEPOINT is ever used. Ordinary constraint failures must not
## tear the batch, and the write statements are written so they cannot fail on a
## constraint: `insert_like` is `INSERT ... SELECT ... WHERE EXISTS(...)
## ON CONFLICT DO NOTHING` (a missing post inserts zero rows, a duplicate inserts
## zero rows) and `insert_post` has no unique constraint to violate. Any OTHER
## (unexpected) error aborts the whole batch via `abortBatch`/`commitBatch`
## rollback, and the server turns every response produced against that batch into
## a 500 -- it can never answer success for a write that did not commit.

import std/[os, strutils]

import ./sqlite3
import ./model

type
  DbError* = object of CatchableError

  BatchState = enum
    bsNone    ## no transaction open (autocommit)
    bsOpen    ## BEGIN IMMEDIATE issued, not yet committed
    bsFailed  ## the batch hit an unexpected error; COMMIT must report failure

  Db* = ref object
    handle: ptr Sqlite3
    stmtGetPost: ptr Sqlite3Stmt
    stmtFeed: ptr Sqlite3Stmt
    stmtFeedIds: ptr Sqlite3Stmt
    stmtFeedLikes: ptr Sqlite3Stmt
    stmtFeedRows: ptr Sqlite3Stmt
    stmtInsertPost: ptr Sqlite3Stmt
    stmtCreatedAt: ptr Sqlite3Stmt
    stmtInsertLike: ptr Sqlite3Stmt
    stmtExistsPost: ptr Sqlite3Stmt
    stmtHealth: ptr Sqlite3Stmt
    groupCommit: bool
    batchState: BatchState
    dirty: bool          ## a write happened since the last checkpoint

  LikeOutcome* = enum
    loLiked, loAlready, loNoPost

const
  SqlSelectPost = """
SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
FROM posts p JOIN users u ON u.id = p.user_id
WHERE p.id = ?"""

  SqlSelectFeed = """
SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
FROM posts p JOIN users u ON u.id = p.user_id
ORDER BY p.created_at DESC, p.id DESC
LIMIT 20"""

  # Dense-range scan (ported from PR #82). The feed is `ORDER BY created_at
  # DESC, id DESC LIMIT 20`; when those top-20 ids fit in a small dense window
  # the ordered scan plus correlated like_count subquery is replaced by three
  # cheap range lookups keyed on id. SqlSelectFeed (above) stays the decline /
  # fallback path and is used verbatim.
  #
  # 1. The top-20 ids in FEED ORDER. The slot order of this list IS the feed
  #    order, so emitting slots in order reproduces created_at DESC, id DESC.
  SqlFeedIds = """
SELECT id FROM posts
ORDER BY created_at DESC, id DESC
LIMIT 20"""

  # 2. Likes in the id window; the caller increments the slot of each post_id
  #    that belongs to the top-20 set and ignores the rest.
  SqlFeedLikes = """
SELECT post_id FROM likes
WHERE post_id BETWEEN ? AND ?"""

  # 3. Row data for the window, NO ORDER BY (the slot map supplies the order).
  #    Same SELECT/JOIN shape as SqlSelectFeed, minus the like_count subquery
  #    (likes come from SqlFeedLikes) and minus the ORDER BY/LIMIT.
  SqlFeedRows = """
SELECT p.id, p.body, p.created_at, u.username
FROM posts p JOIN users u ON u.id = p.user_id
WHERE p.id BETWEEN ? AND ?"""

  SqlInsertPost = "INSERT INTO posts(user_id, body) VALUES(?, ?)"

  SqlSelectCreatedAt = "SELECT created_at FROM posts WHERE id = ?"

  SqlInsertLike = """
INSERT INTO likes(user_id, post_id)
SELECT ?, ? WHERE EXISTS(SELECT 1 FROM posts WHERE id = ?)
ON CONFLICT DO NOTHING"""

  SqlExistsPost = "SELECT 1 FROM posts WHERE id = ?"

  SqlHealth = "SELECT 1"

# ---------------------------------------------------------------------------
# small helpers

proc errMsg(db: Db): string =
  if db.handle == nil: "database handle is nil"
  else: $sqlite3_errmsg(db.handle)

proc fail(db: Db, what: string) {.noreturn.} =
  raise newException(DbError,
    what & " failed (rc=" & $sqlite3_errcode(db.handle) & "): " & db.errMsg())

proc execPragma(db: Db, pragma: string, tolerate: bool) =
  ## Run a single PRAGMA. When `tolerate`, a failure (e.g. journal_mode already
  ## WAL under a second connection) is ignored instead of aborting open.
  let rc = sqlite3_exec(db.handle, ("PRAGMA " & pragma).cstring, nil, nil, nil)
  if rc != SQLITE_OK and not tolerate:
    db.fail("PRAGMA " & pragma)

proc applyPragmas(db: Db) =
  ## Applied in the order required by the plan; journal_mode is tolerated
  ## because it can fail when another connection already owns the WAL.
  ##
  ## `locking_mode=EXCLUSIVE` is the fastest path: no per-statement lock traffic
  ## once the lock is held.
  db.execPragma("journal_mode=WAL", tolerate = true)
  db.execPragma("synchronous=NORMAL", tolerate = false)
  db.execPragma("busy_timeout=5000", tolerate = false)
  db.execPragma("foreign_keys=ON", tolerate = false)
  db.execPragma("temp_store=MEMORY", tolerate = false)
  db.execPragma("cache_size=-1000", tolerate = false)
  # mmap size is env-tunable (SQLITE_MMAP_SIZE, bytes; default 1 GiB).
  var mmapBytes = 1073741824
  try:
    mmapBytes = parseInt(getEnv("SQLITE_MMAP_SIZE", "1073741824"))
  except ValueError:
    mmapBytes = 1073741824
  db.execPragma("mmap_size=" & $mmapBytes, tolerate = false)
  # WAL autocheckpoint is DISABLED (0): the WAL is checkpointed explicitly, off
  # the write path, by checkpointPassive() once per event-loop pass (~1/s). That
  # keeps a checkpoint fsync out of any single request's latency while
  # journal_size_limit still bounds the WAL file.
  db.execPragma("wal_autocheckpoint=0", tolerate = false)
  db.execPragma("journal_size_limit=67108864", tolerate = false)
  db.execPragma("locking_mode=EXCLUSIVE", tolerate = false)

proc prepare(db: Db, sql: string): ptr Sqlite3Stmt =
  var stmt: ptr Sqlite3Stmt
  let rc = sqlite3_prepare_v3(db.handle, sql.cstring, sql.len.cint,
                              SQLITE_PREPARE_PERSISTENT, addr stmt, nil)
  if rc != SQLITE_OK:
    db.fail("prepare")
  result = stmt

proc colText(stmt: ptr Sqlite3Stmt, i: cint): string =
  ## Copy a TEXT column byte-exactly; values are not guaranteed NUL-terminated,
  ## so length comes from sqlite3_column_bytes (called after column_text).
  let p = sqlite3_column_text(stmt, i)
  let n = sqlite3_column_bytes(stmt, i)
  if p == nil or n <= 0:
    return ""
  result = newString(n)
  copyMem(addr result[0], p, n)

proc readRow(stmt: ptr Sqlite3Stmt): Post =
  result.id = sqlite3_column_int64(stmt, 0)
  result.body = colText(stmt, 1)
  result.createdAt = colText(stmt, 2)
  result.author = colText(stmt, 3)
  result.likeCount = sqlite3_column_int64(stmt, 4)

proc bindInt(stmt: ptr Sqlite3Stmt, idx: cint, v: int64) =
  let rc = sqlite3_bind_int64(stmt, idx, v)
  if rc != SQLITE_OK:
    raise newException(DbError, "sqlite3_bind_int64 failed (rc=" & $rc & ")")

proc bindText(stmt: ptr Sqlite3Stmt, idx: cint, s: string) =
  let rc = sqlite3_bind_text(stmt, idx, s.cstring, s.len.cint, SQLITE_TRANSIENT)
  if rc != SQLITE_OK:
    raise newException(DbError, "sqlite3_bind_text failed (rc=" & $rc & ")")

proc resetStmt(stmt: ptr Sqlite3Stmt) =
  discard sqlite3_reset(stmt)
  discard sqlite3_clear_bindings(stmt)

# ---------------------------------------------------------------------------
# group-commit batch control
#
# One batch == one explicit `BEGIN IMMEDIATE ... COMMIT` covering every write
# made during one event-loop pass. `BEGIN IMMEDIATE` (not a deferred BEGIN) takes
# the write lock up front, so a write inside the pass can never be split across
# transactions. Reads issued while the batch is open share the transaction and
# therefore observe the batch's own writes.

proc setGroupCommit*(db: Db, on: bool) =
  ## Enable/disable write batching. Called once at startup from the GROUP_COMMIT
  ## environment flag (default ON in the server). Disabling it restores the old
  ## autocommit behavior byte-for-byte.
  db.groupCommit = on

proc batchOpen*(db: Db): bool =
  ## True while a batch (open OR failed-but-not-yet-finalized) is in effect. The
  ## server uses this to decide whether a response must be withheld until commit.
  db.batchState != bsNone

proc beginBatch*(db: Db) =
  ## Start the pass's transaction on the first write. Idempotent: a second write
  ## in the same pass is a no-op. A failed open marks the batch failed so nothing
  ## in the pass can be reported as a success.
  if db.batchState != bsNone or not db.groupCommit:
    return
  let rc = sqlite3_exec(db.handle, "BEGIN IMMEDIATE".cstring, nil, nil, nil)
  if rc == SQLITE_OK:
    db.batchState = bsOpen
  else:
    db.batchState = bsFailed

proc abortBatch*(db: Db) =
  ## Give up on the current batch: roll it back (releasing the write lock and
  ## discarding every uncommitted write) and remember the failure so `commitBatch`
  ## reports false. Safe to call when no batch is open.
  if db.batchState == bsOpen:
    discard sqlite3_exec(db.handle, "ROLLBACK".cstring, nil, nil, nil)
  if db.batchState != bsNone:
    db.batchState = bsFailed

proc commitBatch*(db: Db): bool =
  ## End the pass's batch. Returns true only when every write in the batch is
  ## durable. On any error it ROLLBACKs and returns false, so the caller must not
  ## treat any write in the batch as committed.
  case db.batchState
  of bsNone:
    result = true
  of bsFailed:
    db.batchState = bsNone
    result = false
  of bsOpen:
    let rc = sqlite3_exec(db.handle, "COMMIT".cstring, nil, nil, nil)
    if rc == SQLITE_OK:
      db.batchState = bsNone
      result = true
    else:
      discard sqlite3_exec(db.handle, "ROLLBACK".cstring, nil, nil, nil)
      db.batchState = bsNone
      result = false

proc checkpointPassive*(db: Db) =
  ## Run a PASSIVE WAL checkpoint, off the request path. Called at most ~1/s from
  ## the event loop after a write. No-op while a group-commit batch is open or
  ## when nothing has been written since the last checkpoint. PASSIVE never
  ## blocks on readers, so it cannot stall the single-threaded loop.
  if db.handle == nil or not db.dirty or db.batchOpen():
    return
  db.dirty = false
  discard sqlite3_wal_checkpoint_v2(db.handle, nil, SQLITE_CHECKPOINT_PASSIVE,
                                    nil, nil)

# ---------------------------------------------------------------------------
# public API

# Statement lifetime (why single-row reads are reset immediately)
# ---------------------------------------------------------------------------
# `sqlite3_step` returning SQLITE_ROW leaves the statement ACTIVE, and an active
# statement keeps the connection's implicit read transaction (and therefore its
# snapshot) open until the statement is reset, finalized, or stepped to
# completion. Single-row readers below therefore reset the statement as soon as
# they have copied the row they need, so the snapshot is released at end-of-use
# instead of lingering until the next statement is stepped.

proc openDb*(path: string): Db =
  ## Open the shared database file, always with `locking_mode=EXCLUSIVE`.
  ##
  ## The bundled SQLite is compiled with SQLITE_OMIT_AUTOINIT (see sqlite3.nim),
  ## so the library is initialized exactly once, here, before the first call into
  ## it. sqlite3_initialize() is idempotent and is the ONLY SQLite entry point
  ## that may run before this call returns.
  let irc = sqlite3_initialize()
  if irc != SQLITE_OK:
    raise newException(DbError, "sqlite3_initialize failed (rc=" & $irc & ")")

  new(result)
  let rc = sqlite3_open_v2(path.cstring, addr result.handle,
                           SQLITE_OPEN_READWRITE or SQLITE_OPEN_NOMUTEX, nil)
  if rc != SQLITE_OK:
    let msg = result.errMsg()
    if result.handle != nil:
      discard sqlite3_close_v2(result.handle)
    raise newException(DbError, "sqlite3_open_v2 failed (rc=" & $rc & "): " & msg)

  result.applyPragmas()

  result.stmtGetPost = result.prepare(SqlSelectPost)
  result.stmtFeed = result.prepare(SqlSelectFeed)
  result.stmtFeedIds = result.prepare(SqlFeedIds)
  result.stmtFeedLikes = result.prepare(SqlFeedLikes)
  result.stmtFeedRows = result.prepare(SqlFeedRows)
  result.stmtInsertPost = result.prepare(SqlInsertPost)
  result.stmtCreatedAt = result.prepare(SqlSelectCreatedAt)
  result.stmtInsertLike = result.prepare(SqlInsertLike)
  result.stmtExistsPost = result.prepare(SqlExistsPost)
  result.stmtHealth = result.prepare(SqlHealth)

proc health*(db: Db): bool =
  let stmt = db.stmtHealth
  resetStmt(stmt)
  result = sqlite3_step(stmt) == SQLITE_ROW
  # Release the implicit read transaction now that we have the row. See the
  # note on statement lifetime above `openDb`.
  resetStmt(stmt)

proc getPost*(db: Db, id: int64, post: var Post): bool =
  let stmt = db.stmtGetPost
  resetStmt(stmt)
  bindInt(stmt, 1, id)
  let rc = sqlite3_step(stmt)
  if rc == SQLITE_ROW:
    post = readRow(stmt)
    result = true
  elif rc == SQLITE_DONE:
    result = false
  else:
    db.fail("getPost step")
  resetStmt(stmt)

proc feedFallback(db: Db, posts: var seq[Post]) =
  ## The ORIGINAL feed query, kept verbatim as the decline path:
  ## ORDER BY created_at DESC, id DESC LIMIT 20 with the correlated like_count
  ## subquery. Byte-identical to the query it replaced.
  posts.setLen(0)
  let stmt = db.stmtFeed
  resetStmt(stmt)
  while true:
    let rc = sqlite3_step(stmt)
    if rc == SQLITE_ROW:
      posts.add readRow(stmt)
    elif rc == SQLITE_DONE:
      break
    else:
      db.fail("feed step")

proc feedRange(db: Db, posts: var seq[Post]): bool =
  ## Dense-range scan (ported from PR #82). Returns true when it reproduced the
  ## feed into `posts`, false to DECLINE and let the caller run feedFallback.
  ##
  ## Declines (returns false) when:
  ##   * (maxId - minId) >= 256  : the top-20 ids are not dense, the window
  ##     would scan too many unrelated rows.
  ##   * filled != n            : some slot never got a row (e.g. a top id whose
  ##     join row is absent), so the scan is not a faithful replacement.
  ##
  ## On success the emit order is slot order 0..n-1, which is exactly the id
  ## order returned by SqlFeedIds, i.e. created_at DESC, id DESC.
  const MaxSpan = 256            # window is [minId, maxId], span must be < 256

  # 1. top-20 ids, in feed order.
  let idsStmt = db.stmtFeedIds
  resetStmt(idsStmt)
  var topIds: array[20, int64]
  var n = 0
  while true:
    let rc = sqlite3_step(idsStmt)
    if rc == SQLITE_ROW:
      topIds[n] = sqlite3_column_int64(idsStmt, 0)
      inc n
    elif rc == SQLITE_DONE:
      break
    else:
      db.fail("feed ids step")
  resetStmt(idsStmt)

  if n == 0:
    # Empty posts table: same as the old LIMIT-20 query returning nothing.
    posts.setLen(0)
    return true

  # 2. window bounds.
  var minId = topIds[0]
  var maxId = topIds[0]
  for i in 1 ..< n:
    if topIds[i] < minId: minId = topIds[i]
    if topIds[i] > maxId: maxId = topIds[i]

  # 3. density gate.
  if maxId - minId >= MaxSpan:
    return false

  # 4. slot map: id offset in the window -> feed position (0..n-1).
  var slotOf: array[MaxSpan, int]
  for i in 0 ..< MaxSpan: slotOf[i] = -1
  var likeFor: array[20, int64]
  for i in 0 ..< n:
    slotOf[int(topIds[i] - minId)] = i
    likeFor[i] = 0

  # 5. likes in the window; count only ids that own a slot.
  let lkStmt = db.stmtFeedLikes
  resetStmt(lkStmt)
  bindInt(lkStmt, 1, minId)
  bindInt(lkStmt, 2, maxId)
  while true:
    let rc = sqlite3_step(lkStmt)
    if rc == SQLITE_ROW:
      let pid = sqlite3_column_int64(lkStmt, 0)
      let slot = slotOf[int(pid - minId)]
      if slot >= 0:
        inc likeFor[slot]
    elif rc == SQLITE_DONE:
      break
    else:
      db.fail("feed likes step")
  resetStmt(lkStmt)

  # 6. rows in the window; place each into its slot. No ORDER BY: ordering comes
  #    from the slot map, so this is a plain range scan.
  var slots: array[20, Post]
  var filledFlag: array[20, bool]
  for i in 0 ..< 20: filledFlag[i] = false
  var filled = 0
  let rwStmt = db.stmtFeedRows
  resetStmt(rwStmt)
  bindInt(rwStmt, 1, minId)
  bindInt(rwStmt, 2, maxId)
  while true:
    let rc = sqlite3_step(rwStmt)
    if rc == SQLITE_ROW:
      let pid = sqlite3_column_int64(rwStmt, 0)
      let slot = slotOf[int(pid - minId)]
      if slot >= 0:
        var p: Post
        p.id = pid
        p.body = colText(rwStmt, 1)
        p.createdAt = colText(rwStmt, 2)
        p.author = colText(rwStmt, 3)
        p.likeCount = 0            # filled from likeFor[] below
        slots[slot] = p
        if not filledFlag[slot]:
          filledFlag[slot] = true
          inc filled
    elif rc == SQLITE_DONE:
      break
    else:
      db.fail("feed rows step")
  resetStmt(rwStmt)

  # 7. every slot must be filled, else the scan is not equivalent.
  if filled != n:
    return false

  # 8. emit in slot order, attaching the like counts gathered in step 5.
  posts.setLen(n)
  for i in 0 ..< n:
    slots[i].likeCount = likeFor[i]
    posts[i] = slots[i]
  return true

proc feed*(db: Db, posts: var seq[Post]) =
  ## Fill `posts` in feed order (created_at DESC, id DESC, up to 20 entries).
  ##
  ## Prefers the dense-range scan; on decline it runs the original ordered query.
  ## Both paths produce byte-identical results, so the serialized /feed body is
  ## unchanged.
  if db.feedRange(posts):
    return
  db.feedFallback(posts)

proc createPost*(db: Db, userId: int64, username, body: string,
                 post: var Post): bool =
  ## INSERT. Without group commit this is autocommit (durable before returning).
  ## With group commit the row joins the pass's transaction and is durable only
  ## after the server COMMITs; the server withholds the response until then.
  if db.groupCommit:
    db.beginBatch()
    # A batch that already failed must not run further writes: answering the
    # request 500 is correct, touching the database is not.
    if db.batchState == bsFailed:
      return false
  let ins = db.stmtInsertPost
  resetStmt(ins)
  bindInt(ins, 1, userId)
  bindText(ins, 2, body)
  let rc = sqlite3_step(ins)
  if rc != SQLITE_DONE:
    # Statement finished (no reset-with-error); a failed insert is not a
    # success. Only surface it as false, per the frozen bool contract. Under
    # group commit this is an unexpected error, so abort the whole batch.
    if db.groupCommit:
      db.abortBatch()
    return false

  let newId = sqlite3_last_insert_rowid(db.handle)

  let ca = db.stmtCreatedAt
  resetStmt(ca)
  bindInt(ca, 1, newId)
  var createdAt = ""
  let crc = sqlite3_step(ca)
  if crc != SQLITE_ROW:
    db.fail("createPost re-read created_at")
  createdAt = colText(ca, 0)
  resetStmt(ca)          # release the read snapshot (see the note above openDb)

  post.id = newId
  post.body = body
  post.createdAt = createdAt
  post.author = username
  post.likeCount = 0
  db.dirty = true
  result = true

proc like*(db: Db, userId, postId: int64): LikeOutcome =
  ## INSERT ... ON CONFLICT DO NOTHING. Without group commit this is autocommit
  ## (durable before returning). With group commit the like joins the pass's
  ## transaction. The statement inserts at most one row and can never hit a
  ## constraint (the post is checked with WHERE EXISTS first, and the unique
  ## (user_id, post_id) conflict is swallowed by ON CONFLICT DO NOTHING), so it
  ## cannot tear the batch.
  if db.groupCommit:
    db.beginBatch()
    if db.batchState == bsFailed:
      # The batch is already lost; do not execute. Raising maps to a 500.
      raise newException(DbError, "group-commit batch already failed")
  let ins = db.stmtInsertLike
  resetStmt(ins)
  bindInt(ins, 1, userId)
  bindInt(ins, 2, postId)
  bindInt(ins, 3, postId)
  let rc = sqlite3_step(ins)
  if rc != SQLITE_DONE:
    if db.groupCommit:
      db.abortBatch()
    db.fail("like insert")

  if sqlite3_changes(db.handle) == 1:
    db.dirty = true
    return loLiked

  let ex = db.stmtExistsPost
  resetStmt(ex)
  bindInt(ex, 1, postId)
  let erc = sqlite3_step(ex)
  if erc == SQLITE_ROW:
    resetStmt(ex)        # release the read snapshot (see the note above openDb)
    return loAlready
  elif erc == SQLITE_DONE:
    return loNoPost
  else:
    db.fail("like existence check")
