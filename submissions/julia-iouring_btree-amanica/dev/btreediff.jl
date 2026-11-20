# Dev-only differential test: the B-tree backend (csrc/btree.c) against the SQL backend on the same
# database, response bytes compared. Runs inside the dev container: dev/ct.sh btreediff.
# Covers GET /feed, GET /posts/:id (existing, missing, new) and the like check, in autocommit mode
# (each read its own transaction) and inside the pass's transaction (group commit), before and after
# inserts, with likes on the newest posts, and after a checkpoint; also non-contiguous newest ids.
include(joinpath(@__DIR__, "..", "src", "JuliaRaw.jl"))
const J = JuliaRaw
using Random

json(f) = (b = J.Buf(); r = f(b); (r, String(b.data[1:b.len])))
const SQL = J.SqlBackend()
const BT = J.BtreeBackend()
fails = Ref(0)
checks = Ref(0)

function same(what, a, b)
    checks[] += 1
    a == b && return
    fails[] += 1
    fails[] <= 10 && println("MISMATCH $what:\n  sql:   $(first(string(a), 400))\n  btree: $(first(string(b), 400))")
end

function check_reads(tag, rng, maxid)
    same("$tag feed", json(b -> J.feed!(SQL, b)), json(b -> J.feed!(BT, b)))
    for id in vcat([1, 2, maxid, maxid - 1, maxid + 1, maxid + 1000, 10^12], rand(rng, 1:maxid, 300))
        same("$tag post $id", json(b -> J.post!(SQL, b, id)), json(b -> J.post!(BT, b, id)))
        same("$tag exists $id", J.post_exists(SQL, id), J.post_exists(BT, id))
    end
end

# The event loop's pattern: reads and writes of one pass in one transaction, committed at the end.
function pass!(f)
    J.GROUP_ON[] = true
    try
        f()
    finally
        J.IN_TXN[] && J.txn_commit!()
        J.GROUP_ON[] = false
    end
end

function main(path)
    J.db_open(path)
    bench("fresh seed")
    rng = MersenneTwister(1)
    st_max = J.db_prepare("SELECT max(id) FROM posts")
    maxid() = (J.step(st_max); v = J.col_int(st_max, 0); J.reset(st_max); v)
    m = maxid()
    check_reads("seed", rng, m)
    pass!(() -> check_reads("seed-txn", rng, m))
    # inserts and likes in passes, reads interleaved (read txn -> write txn -> reads in the write txn)
    author = J.Buf(); J.putstr!(author, "user7")
    for p in 1:50
        pass!() do
            same("p$p feed pre", json(b -> J.feed!(SQL, b)), json(b -> J.feed!(BT, b)))
            for k in 1:3
                body = "post $p.$k \"quoted\" \\ back\nslash é ✓ " * "x"^rand(rng, 0:400)
                GC.@preserve body J.create_post!(J.Buf(), rand(rng, 1:20000), pointer(body), sizeof(body), author)
            end
            mm = maxid()
            for k in 1:10
                J.insert_like(rand(rng, 1:20000), mm - rand(rng, 0:25))
            end
            same("p$p feed post-write", json(b -> J.feed!(SQL, b)), json(b -> J.feed!(BT, b)))
            for id in (mm, mm - 1, mm - 5, mm + 1)
                same("p$p post $id", json(b -> J.post!(SQL, b, id)), json(b -> J.post!(BT, b, id)))
            end
        end
    end
    m = maxid()
    check_reads("after-writes", rng, m)
    J.db_exec("PRAGMA wal_checkpoint(TRUNCATE)")
    check_reads("after-checkpoint", rng, m)
    # newest ids not contiguous: a post dated in the future, and one deleted from the middle (dev db only)
    J.db_exec("INSERT INTO posts (user_id, body, created_at) VALUES (3, 'from the future', '2999-01-01T00:00:00.000Z')")
    J.db_exec("DELETE FROM posts WHERE id = $(m - 3)")
    check_reads("non-contiguous", rng, m + 1)
    pass!(() -> check_reads("non-contiguous-txn", rng, m + 1))
    # a post whose user is missing: the join drops it, so the B-tree feed falls back to SQL
    J.db_exec("PRAGMA foreign_keys=OFF")
    J.db_exec("INSERT INTO posts (user_id, body) VALUES (999999999, 'orphan')")
    check_reads("orphan", rng, m + 2)
    println("btreediff: $(checks[]) checks, $(fails[]) mismatches, B-tree reads=$(ccall((:jr_bt_reads, J.LIBJR), Clong, ())) errors=$(ccall((:jr_bt_errors, J.LIBJR), Clong, ()))")
    bench("after the writes (newest ids not contiguous)")
    fails[] == 0 || exit(1)
end


# Timing (hot cache, one thread): per-call cost of each backend, in the pass's transaction as on the loop.
function bench(tag)
    J.GROUP_ON[] = true
    b = J.Buf()
    for (name, be) in (("sql", SQL), ("btree", BT))
        for (what, f) in (("feed", () -> J.feed!(be, b)), ("post", () -> J.post!(be, b, rand(1:500000))),
                          ("exists", () -> J.post_exists(be, rand(1:500000))))
            for _ in 1:2000; J.reset!(b); f(); end
            n = what == "feed" ? 20000 : 100000
            t0 = time_ns()
            for i in 1:n
                J.reset!(b); f()
                i % 64 == 0 && J.IN_TXN[] && J.txn_commit!()     # ~64 requests per loop pass
            end
            J.IN_TXN[] && J.txn_commit!()
            println("bench [$tag] $name $what: $(round((time_ns() - t0) / n / 1000, digits = 2)) us")
        end
    end
    J.GROUP_ON[] = false
end

main(ARGS[1])
