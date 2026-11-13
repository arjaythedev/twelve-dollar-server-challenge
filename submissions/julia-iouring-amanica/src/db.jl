# SQLite through its C API (build/libjr.so = the pinned amalgamation), plus the read-backend interface.

const LIBJR = normpath(joinpath(@__DIR__, "..", "build", "libjr.so"))

const CKPT_PAGES = 1000     # WAL size that wakes the checkpoint thread (SQLite's autocheckpoint default)
# WAL size at which writes wait until the checkpoint has caught up (62 MB, as in fsharp-kestrel-delneg).
# JR_HOLD_PAGES overrides it for dev A/B (0: never hold).
const HOLD_PAGES = parse(Int, let v = get(ENV, "JR_HOLD_PAGES", ""); isempty(v) ? "15000" : v end)
const SQLITE_ROW = Cint(100)
const SQLITE_DONE = Cint(101)

const DB = Ref{Ptr{Cvoid}}(C_NULL)
const ST_HEALTH = Ref{Ptr{Cvoid}}(C_NULL)
const ST_FEED = Ref{Ptr{Cvoid}}(C_NULL)
const ST_POST = Ref{Ptr{Cvoid}}(C_NULL)
const ST_EXISTS = Ref{Ptr{Cvoid}}(C_NULL)
const ST_INSERT_POST = Ref{Ptr{Cvoid}}(C_NULL)
const ST_INSERT_LIKE = Ref{Ptr{Cvoid}}(C_NULL)
const ST_FEED_IDS = Ref{Ptr{Cvoid}}(C_NULL)
const ST_FEED_RANGE = Ref{Ptr{Cvoid}}(C_NULL)
const ST_FEED_LIKES = Ref{Ptr{Cvoid}}(C_NULL)
const FEED_COUNTS = zeros(Int, 20)
# GET /feed by range scans when the 20 newest posts have contiguous ids (see feed!). JR_FEED=plain: always
# the single query, for dev A/B.
const FEED_RANGE = get(ENV, "JR_FEED", "") != "plain"
const ST_BEGIN = Ref{Ptr{Cvoid}}(C_NULL)
const ST_COMMIT = Ref{Ptr{Cvoid}}(C_NULL)

# Group commit (io_uring only): the writes of one event-loop pass share one transaction, committed before the
# loop next submits to the kernel, so every reply waits for its commit (rule 6). JR_GROUP=off: one commit per
# write, for dev A/B.
const GROUP = get(ENV, "JR_GROUP", "") != "off"
const GROUP_ON = Ref(false)         # set by the transport that commits before it submits
const IN_TXN = Ref(false)

const POST_SELECT = """
SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
  FROM posts p JOIN users u ON u.id = p.user_id"""

db_errmsg() = unsafe_string(ccall((:sqlite3_errmsg, LIBJR), Cstring, (Ptr{Cvoid},), DB[]))

function db_exec(sql::String)
    rc = ccall((:sqlite3_exec, LIBJR), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Cvoid}),
        DB[], sql, C_NULL, C_NULL, C_NULL)
    rc == 0 || error("sqlite: $sql: $(db_errmsg())")
end

function db_prepare(sql::String)
    pp = Ref{Ptr{Cvoid}}(C_NULL)
    rc = ccall((:sqlite3_prepare_v3, LIBJR), Cint,
        (Ptr{Cvoid}, Cstring, Cint, Cuint, Ref{Ptr{Cvoid}}, Ptr{Cvoid}), DB[], sql, -1, 0x01, pp, C_NULL)
    rc == 0 || error("sqlite prepare: $sql: $(db_errmsg())")
    pp[]
end

function db_open(path::String)
    pp = Ref{Ptr{Cvoid}}(C_NULL)
    rc = ccall((:sqlite3_open_v2, LIBJR), Cint, (Cstring, Ref{Ptr{Cvoid}}, Cint, Ptr{Cvoid}),
        path, pp, 0x00000002 | 0x00008000, C_NULL)        # READWRITE | NOMUTEX
    DB[] = pp[]
    rc == 0 || error("sqlite open $path: $(db_errmsg())")
    db_exec("PRAGMA journal_mode=WAL")
    db_exec("PRAGMA synchronous=NORMAL")
    db_exec("PRAGMA busy_timeout=5000")
    db_exec("PRAGMA cache_size=-65536")
    db_exec("PRAGMA mmap_size=268435456")
    db_exec("PRAGMA temp_store=MEMORY")
    ST_HEALTH[] = db_prepare("SELECT 1")
    ST_FEED[] = db_prepare(POST_SELECT * " ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
    ST_FEED_IDS[] = db_prepare("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20")
    ST_FEED_RANGE[] = db_prepare("""SELECT p.id, p.body, p.created_at, u.username
        FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC""")
    ST_FEED_LIKES[] = db_prepare("SELECT post_id, count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id")
    ST_POST[] = db_prepare(POST_SELECT * " WHERE p.id = ?1")
    ST_EXISTS[] = db_prepare("SELECT 1 FROM posts WHERE id = ?1")
    ST_INSERT_POST[] = db_prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at")
    ST_INSERT_LIKE[] = db_prepare("INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING")
    ST_BEGIN[] = db_prepare("BEGIN")
    ST_COMMIT[] = db_prepare("COMMIT")
    nothing
end

# Before a write: opens the pass's transaction when grouping. false on a database error.
@inline function txn_begin!()
    (GROUP_ON[] && !IN_TXN[]) || return true
    rc = step(ST_BEGIN[])
    reset(ST_BEGIN[])
    rc == SQLITE_DONE || return false
    IN_TXN[] = true
    true
end

# Commits the open transaction. Replies already written claim these rows, so if the commit fails (or an
# error rolled the transaction back early) the process stops before submitting them: no reply then claims a
# write that isn't committed.
function txn_commit!()
    @dbg t0 = time_ns()
    IN_TXN[] = false
    ccall((:sqlite3_get_autocommit, LIBJR), Cint, (Ptr{Cvoid},), DB[]) == 0 ||
        error("group commit: the transaction was rolled back: $(db_errmsg())")
    rc = step(ST_COMMIT[])
    reset(ST_COMMIT[])
    rc == SQLITE_DONE || error("group commit failed: $(db_errmsg())")
    @dbg MAX_WRITE_NS[] = max(MAX_WRITE_NS[], time_ns() - t0)
    nothing
end

# Checkpoints on a background thread (csrc/shim.c): on a real disk they blocked the event loop ~65% of the
# time. Only for a transport that can hold writes (io_uring): otherwise the WAL would grow without limit, so
# epoll keeps SQLite's automatic checkpoint. JR_CKPT=auto forces that too (dev A/B only).
function ckpt_start(path::String)
    get(ENV, "JR_CKPT", "") == "auto" && return false
    restart = get(ENV, "JR_RESTART", "") == "loop" ? 0 : 1   # dev A/B only: who restarts the WAL after a hold
    rc = ccall((:jr_ckpt_start, LIBJR), Cint, (Ptr{Cvoid}, Cstring, Cint, Cint, Cint), DB[], path, CKPT_PAGES, HOLD_PAGES, restart)
    rc == 0 || error("checkpoint thread: sqlite error $rc")
    true
end
@inline write_hold() = ccall((:jr_ckpt_hold, LIBJR), Cint, ()) != 0

@inline step(st::Ptr{Cvoid}) = ccall((:sqlite3_step, LIBJR), Cint, (Ptr{Cvoid},), st)
@inline reset(st::Ptr{Cvoid}) = ccall((:sqlite3_reset, LIBJR), Cint, (Ptr{Cvoid},), st)
@inline bind_int(st::Ptr{Cvoid}, i::Int, v::Int) =
    ccall((:sqlite3_bind_int64, LIBJR), Cint, (Ptr{Cvoid}, Cint, Int64), st, i, v)
@inline bind_text(st::Ptr{Cvoid}, i::Int, p::Ptr{UInt8}, n::Int) =     # SQLITE_STATIC: p outlives the step
    ccall((:sqlite3_bind_text, LIBJR), Cint, (Ptr{Cvoid}, Cint, Ptr{UInt8}, Cint, Ptr{Cvoid}), st, i, p, n, C_NULL)
@inline col_int(st::Ptr{Cvoid}, i::Int) = Int(ccall((:sqlite3_column_int64, LIBJR), Int64, (Ptr{Cvoid}, Cint), st, i))
@inline col_text(st::Ptr{Cvoid}, i::Int) = ccall((:sqlite3_column_text, LIBJR), Ptr{UInt8}, (Ptr{Cvoid}, Cint), st, i)
@inline col_bytes(st::Ptr{Cvoid}, i::Int) = Int(ccall((:sqlite3_column_bytes, LIBJR), Cint, (Ptr{Cvoid}, Cint), st, i))
@inline db_changes() = Int(ccall((:sqlite3_changes, LIBJR), Cint, (Ptr{Cvoid},), DB[]))

@inline function put_text_col!(b::Buf, st::Ptr{Cvoid}, i::Int)
    p = col_text(st, i)             # text before bytes, as the SQLite docs recommend
    putjsonstr!(b, p, col_bytes(st, i))
end

# Post object from a POST_SELECT row.
function put_post_row!(b::Buf, st::Ptr{Cvoid})
    putstr!(b, "{\"id\":"); putint!(b, col_int(st, 0))
    putstr!(b, ",\"body\":"); put_text_col!(b, st, 1)
    putstr!(b, ",\"created_at\":"); put_text_col!(b, st, 2)
    putstr!(b, ",\"author\":"); put_text_col!(b, st, 3)
    putstr!(b, ",\"like_count\":"); putint!(b, col_int(st, 4))
    pushb!(b, UInt8('}'))
end

# ---- reads, through SQLite's public API only (prepared statements). Each writes response JSON straight into b. ----

abstract type ReadBackend end
struct SqlBackend <: ReadBackend end

# {"posts":[...]} -> true, or false on a database error.
#
# Fast path: the 20 newest ids come from the covering index (created_at DESC, id DESC). Usually they are
# hi, hi-1, ..., hi-19 (ids and created_at both grow with each insert); then the same posts in the same order
# are `id BETWEEN hi-19 AND hi ORDER BY id DESC`, and their like counts one range scan of likes_post_id_idx:
# 1 descent instead of 20 for the counts. Measured (dev/feedbench.jl, WAL of 7.5k-15k frames): 25-31 ->
# 18-20 us per feed, same JSON. Otherwise, or if a row is missing, the plain query. All three statements
# read the same data: only this thread writes, and not in between.
function feed!(r::SqlBackend, b::Buf)
    FEED_RANGE || return feed_plain!(b)
    ids = ST_FEED_IDS[]
    hi = -1; k = 0
    while true
        rc = step(ids)
        rc == SQLITE_ROW || (reset(ids); rc == SQLITE_DONE || return false; break)
        id = col_int(ids, 0)
        k == 0 && (hi = id)
        id == hi - k || (reset(ids); return feed_plain!(b))
        k += 1
    end
    k == 20 || return feed_plain!(b)
    lo = hi - 19
    fill!(FEED_COUNTS, 0)
    st = ST_FEED_LIKES[]
    bind_int(st, 1, lo); bind_int(st, 2, hi)
    while true
        rc = step(st)
        rc == SQLITE_ROW || (reset(st); rc == SQLITE_DONE || return false; break)
        @inbounds FEED_COUNTS[col_int(st, 0) - lo + 1] = col_int(st, 1)
    end
    st = ST_FEED_RANGE[]
    bind_int(st, 1, lo); bind_int(st, 2, hi)
    start = b.len
    putstr!(b, "{\"posts\":[")
    n = 0
    while true
        rc = step(st)
        rc == SQLITE_ROW || (reset(st); rc == SQLITE_DONE || return false; break)
        n > 0 && pushb!(b, UInt8(','))
        id = col_int(st, 0)
        putstr!(b, "{\"id\":"); putint!(b, id)
        putstr!(b, ",\"body\":"); put_text_col!(b, st, 1)
        putstr!(b, ",\"created_at\":"); put_text_col!(b, st, 2)
        putstr!(b, ",\"author\":"); put_text_col!(b, st, 3)
        putstr!(b, ",\"like_count\":"); @inbounds putint!(b, FEED_COUNTS[id - lo + 1])
        pushb!(b, UInt8('}'))
        n += 1
    end
    n == 20 || (b.len = start; return feed_plain!(b))
    putstr!(b, "]}")
    true
end

function feed_plain!(b::Buf)
    st = ST_FEED[]
    putstr!(b, "{\"posts\":[")
    first = true
    while true
        rc = step(st)
        rc == SQLITE_ROW || (reset(st); return rc == SQLITE_DONE && (putstr!(b, "]}"); true))
        first || pushb!(b, UInt8(','))
        first = false
        put_post_row!(b, st)
    end
end

# {"post":{...}} -> 1 found, 0 not found, -1 error.
function post!(::SqlBackend, b::Buf, id::Int)
    st = ST_POST[]
    bind_int(st, 1, id)
    rc = step(st)
    if rc == SQLITE_ROW
        putstr!(b, "{\"post\":")
        put_post_row!(b, st)
        pushb!(b, UInt8('}'))
        reset(st)
        return 1
    end
    reset(st)
    rc == SQLITE_DONE ? 0 : -1
end

# 1 exists, 0 not found, -1 error.
function post_exists(::SqlBackend, id::Int)
    st = ST_EXISTS[]
    bind_int(st, 1, id)
    rc = step(st)
    reset(st)
    rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1
end

const READ = SqlBackend()

# ---- writes (always SQL) ----

# Inserts a post and writes {"post":{...}}. Returns false on a database error.
function create_post!(b::Buf, uid::Int, p::Ptr{UInt8}, n::Int, author::Buf)
    @dbg t0 = time_ns()
    txn_begin!() || return false
    st = ST_INSERT_POST[]
    bind_int(st, 1, uid)
    bind_text(st, 2, p, n)
    rc = step(st)
    if rc != SQLITE_ROW
        reset(st)
        return false
    end
    putstr!(b, "{\"post\":{\"id\":"); putint!(b, col_int(st, 0))
    putstr!(b, ",\"body\":"); putjsonstr!(b, p, n)
    putstr!(b, ",\"created_at\":"); put_text_col!(b, st, 1)
    putstr!(b, ",\"author\":"); putjsonstr!(b, author)
    putstr!(b, ",\"like_count\":0}}")
    rc = step(st)                   # run to completion: the autocommit happens here
    reset(st)
    @dbg MAX_WRITE_NS[] = max(MAX_WRITE_NS[], time_ns() - t0)
    rc == SQLITE_DONE
end

const MAX_WRITE_NS = Ref(UInt64(0))     # --debug: longest single write on the event loop in this stats window

# 1 new like, 2 already liked, -1 error.
function insert_like(uid::Int, pid::Int)
    @dbg t0 = time_ns()
    txn_begin!() || return -1
    st = ST_INSERT_LIKE[]
    bind_int(st, 1, uid)
    bind_int(st, 2, pid)
    rc = step(st)
    reset(st)
    @dbg MAX_WRITE_NS[] = max(MAX_WRITE_NS[], time_ns() - t0)
    rc == SQLITE_DONE || return -1
    db_changes() == 1 ? 1 : 2
end

function health_ok()
    st = ST_HEALTH[]
    rc = step(st)
    reset(st)
    rc == SQLITE_ROW
end
