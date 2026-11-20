# Dev-only microbenchmark of GET /feed query variants, in-process on a copy of the seed (PLAN 5.5).
# Runs inside the dev container: dev/ct.sh feedbench. Each variant must give byte-identical JSON to the
# server's current feed!. Timings are hot-cache, one connection: they show how much work a query does,
# not the cold-cache cost under 175k connections.
include(joinpath(@__DIR__, "..", "src", "JuliaRaw.jl"))
const J = JuliaRaw

const N = 20_000

# The 20 newest ids, from the covering index only.
const IDS_SQL = "SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20"
# Rows for a contiguous id range, newest id first: one range scan of the posts table.
const RANGE_SQL = J.POST_SELECT * " WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC"
# Same, with the like counts from one range scan of likes_post_id_idx instead of 20 lookups.
const RANGE_LIKES_SQL = """
SELECT p.id, p.body, p.created_at, u.username, coalesce(c.n, 0)
  FROM posts p JOIN users u ON u.id = p.user_id
  LEFT JOIN (SELECT post_id, count(*) AS n FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id) c
         ON c.post_id = p.id
 WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC"""

# Newest-first ids contiguous (hi, hi-1, ..., hi-19)? Then hi, else -1.
function contiguous_hi(st)
    hi = -1; k = 0
    while J.step(st) == J.SQLITE_ROW
        id = J.col_int(st, 0)
        k == 0 && (hi = id)
        id == hi - k || (J.reset(st); return -1)
        k += 1
    end
    J.reset(st)
    k == 20 ? hi : -1
end

function feed_range!(b::J.Buf, ids, range)
    hi = contiguous_hi(ids)
    hi < 0 && return J.feed_plain!(b)               # fallback: the plain query
    J.bind_int(range, 1, hi - 19); J.bind_int(range, 2, hi)
    J.putstr!(b, "{\"posts\":[")
    n = 0
    while J.step(range) == J.SQLITE_ROW
        n > 0 && J.pushb!(b, UInt8(','))
        J.put_post_row!(b, range); n += 1
    end
    J.reset(range)
    n == 20 || (J.reset!(b); return J.feed_plain!(b))   # a row went missing: fallback
    J.putstr!(b, "]}")
    true
end

# Posts + authors from one range scan, like counts from a second range scan of likes_post_id_idx
# (one descent instead of 20), merged here.
const RANGE_NOLIKES_SQL = "SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC"
const LIKES_RANGE_SQL = "SELECT post_id, count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id"
const COUNTS = zeros(Int, 20)

function feed_two!(b::J.Buf, ids, rows, likes)
    hi = contiguous_hi(ids)
    hi < 0 && return J.feed_plain!(b)
    lo = hi - 19
    fill!(COUNTS, 0)
    J.bind_int(likes, 1, lo); J.bind_int(likes, 2, hi)
    while J.step(likes) == J.SQLITE_ROW
        COUNTS[J.col_int(likes, 0) - lo + 1] = J.col_int(likes, 1)
    end
    J.reset(likes)
    J.bind_int(rows, 1, lo); J.bind_int(rows, 2, hi)
    J.putstr!(b, "{\"posts\":[")
    n = 0
    while J.step(rows) == J.SQLITE_ROW
        n > 0 && J.pushb!(b, UInt8(','))
        id = J.col_int(rows, 0)
        J.putstr!(b, "{\"id\":"); J.putint!(b, id)
        J.putstr!(b, ",\"body\":"); J.put_text_col!(b, rows, 1)
        J.putstr!(b, ",\"created_at\":"); J.put_text_col!(b, rows, 2)
        J.putstr!(b, ",\"author\":"); J.put_text_col!(b, rows, 3)
        J.putstr!(b, ",\"like_count\":"); J.putint!(b, COUNTS[id - lo + 1])
        J.pushb!(b, UInt8('}'))
        n += 1
    end
    J.reset(rows)
    n == 20 || (J.reset!(b); return J.feed_plain!(b))
    J.putstr!(b, "]}")
    true
end

json(f) = (b = J.Buf(); f(b); String(b.data[1:b.len]))

function bench(name, f, ref)
    b = J.Buf()
    for _ in 1:1000; J.reset!(b); f(b); end           # warm up
    t = time_ns()
    for _ in 1:N; J.reset!(b); f(b); end
    us = (time_ns() - t) / N / 1000
    J.reset!(b); al = @allocated f(b)              # bytes for one call, warm
    check = ref === nothing ? "" : json(f) == ref ? "same JSON" : "DIFFERENT JSON"
    println(rpad(name, 24), lpad(string(round(us; digits = 2)), 8), " µs/feed  ", lpad(string(al), 5), " B   ", check)
    check == "DIFFERENT JSON" && println("  got: ", first(json(f), 300), "\n  ref: ", first(ref, 300))
end

function run_all(label)
    println("== ", label)
    ids = J.db_prepare(IDS_SQL); range = J.db_prepare(RANGE_SQL); rl = J.db_prepare(RANGE_LIKES_SQL)
    ref = json(b -> J.feed_plain!(b))
    println("contiguous: ", contiguous_hi(ids) > 0)
    bench("current (feed!)", b -> J.feed_plain!(b), ref)
    bench("ids only (index)", b -> (contiguous_hi(ids); true), nothing)
    bench("ids + range", b -> feed_range!(b, ids, range), ref)
    bench("ids + range + likes", b -> feed_range!(b, ids, rl), ref)
    rn = J.db_prepare(RANGE_NOLIKES_SQL); lr = J.db_prepare(LIKES_RANGE_SQL)
    bench("ids + range, likes apart", b -> feed_two!(b, ids, rn, lr), ref)
    bench("current (again)", b -> J.feed_plain!(b), ref)
    bench("server feed! (src/db.jl)", b -> J.feed!(J.READ, b), ref)
    # Where the time goes (different JSON on purpose): without like counts, without the users join.
    nolikes = J.db_prepare("SELECT p.id, p.body, p.created_at, u.username, 0 FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
    nousers = J.db_prepare("SELECT p.id, p.body, p.created_at, 'x', (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
    norows = J.db_prepare("SELECT p.id, p.body, p.created_at, 'x', 0 FROM posts p ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
    rows!(st) = b -> begin
        while J.step(st) == J.SQLITE_ROW; J.put_post_row!(b, st); end
        J.reset(st); true
    end
    bench("  without like counts", rows!(nolikes), nothing)
    bench("  without users join", rows!(nousers), nothing)
    bench("  posts only", rows!(norows), nothing)
    for st in (ids, range, rl, rn, lr, nolikes, nousers, norows)
        ccall((:sqlite3_finalize, J.LIBJR), Cint, (Ptr{Cvoid},), st)
    end
end

path = ARGS[1]
J.db_open(path)
J.db_exec("PRAGMA wal_autocheckpoint=0")      # keep the WAL, like the server between checkpoints
run_all("empty WAL")
# WALs like the server's between checkpoints (it holds writes at 15,000 pages): likes on recent posts and
# some new posts, committed in small groups like the event loop's passes.
frames() = filesize(path * "-wal") ÷ (4096 + 24)
let i = 0
    for target in (7_500, 15_000)
        while frames() < target
            J.db_exec("BEGIN")
            for _ in 1:20
                i += 1
                J.db_exec("INSERT OR IGNORE INTO likes (user_id, post_id) VALUES ($(rand(1:50_000)), $(rand(499_000:500_000)))")
                i % 50 == 0 && J.db_exec("INSERT INTO posts (user_id, body) VALUES ($(rand(1:50_000)), 'bench post $i')")
            end
            J.db_exec("COMMIT")
        end
        run_all("WAL with ~$(frames()) frames")
    end
end
