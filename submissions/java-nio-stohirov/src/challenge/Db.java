package challenge;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;

final class Db {
    private static final String PRAGMAS = """
            PRAGMA locking_mode = EXCLUSIVE;
            PRAGMA journal_mode = WAL;
            PRAGMA synchronous = NORMAL;
            PRAGMA cache_size = -65536;
            PRAGMA temp_store = MEMORY;
            PRAGMA mmap_size = 0;
            PRAGMA journal_size_limit = 67108864;
            PRAGMA wal_autocheckpoint = 0;
            """;

    private static final String POST_JSON = """
            json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username,
                        'like_count',(SELECT count(*) FROM likes l WHERE l.post_id = p.id))""";

    private static final String FEED_SQL = "SELECT " + POST_JSON
            + " FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20";

    private static final byte[] FEED_OPEN = Out.ascii("{\"posts\":["), FEED_CLOSE = Out.ascii("]}");
    private static final byte[] COMMA = {','};

    private static final String POST_SQL = "SELECT json_object('post', " + POST_JSON + ")"
            + " FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1";

    private static final String INSERT_POST_SQL = """
            INSERT INTO posts (user_id, body) VALUES (?1, ?2)
            RETURNING json_object('post', json_object('id',id,'body',body,'created_at',created_at,
                      'author',?3,'like_count',0))""";

    private static final String LIKE_SQL = """
            INSERT INTO likes (user_id, post_id)
            SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)
            ON CONFLICT (user_id, post_id) DO NOTHING""";

    private final MemorySegment db;
    private final MemorySegment feed, post, insertPost, like, exists, ping, begin, commit, rollback;
    private final MemorySegment scratch = Arena.global().allocate(64 * 1024);
    private final Out feedBody = new Out(64 * 1024);

    private boolean inTxn;
    private int unchecked;

    Db(String path) {
        check(Sqlite.initialize(), "sqlite3_initialize");
        try (Arena a = Arena.ofConfined()) {
            MemorySegment pp = a.allocate(ADDRESS);
            int rc = Sqlite.openV2(a.allocateFrom(path), pp,
                    Sqlite.OPEN_READWRITE | Sqlite.OPEN_NOMUTEX | Sqlite.OPEN_EXRESCODE);
            db = pp.get(ADDRESS, 0);
            if (rc != Sqlite.OK) throw new Sqlite.SqliteException("open " + path + ": " + Sqlite.errmsg(db));
            check(Sqlite.exec(db, a.allocateFrom(PRAGMAS)), "pragmas");
        }
        feed = prepare(FEED_SQL);
        post = prepare(POST_SQL);
        insertPost = prepare(INSERT_POST_SQL);
        like = prepare(LIKE_SQL);
        exists = prepare("SELECT 1 FROM posts WHERE id = ?1");
        ping = prepare("SELECT 1");
        begin = prepare("BEGIN IMMEDIATE");
        commit = prepare("COMMIT");
        rollback = prepare("ROLLBACK");
    }

    private void check(int rc, String what) {
        if (rc != Sqlite.OK) throw new Sqlite.SqliteException(what + ": " + (db == null ? "rc " + rc : Sqlite.errmsg(db)));
    }

    private MemorySegment prepare(String sql) {
        try (Arena a = Arena.ofConfined()) {
            MemorySegment pp = a.allocate(ADDRESS);
            check(Sqlite.prepareV3(db, a.allocateFrom(sql), Sqlite.PREPARE_PERSISTENT, pp), "prepare " + sql);
            return pp.get(ADDRESS, 0);
        }
    }

    private Sqlite.SqliteException error(int rc) {
        return new Sqlite.SqliteException("sqlite rc " + rc + ": " + Sqlite.errmsg(db));
    }

    private boolean jsonRow(MemorySegment st, Out out, byte[] status) {
        try {
            int rc = Sqlite.step(st);
            if (rc == Sqlite.ROW) {
                MemorySegment p = Sqlite.columnText(st, 0);
                int n = Sqlite.columnBytes(st, 0);
                out.head(status, n);
                out.copyNative(p, n);
                return true;
            }
            if (rc == Sqlite.DONE) return false;
            throw error(rc);
        } finally {
            Sqlite.reset(st);
        }
    }

    private void run(MemorySegment st) {
        int rc;
        try {
            rc = Sqlite.step(st);
        } finally {
            Sqlite.reset(st);
        }
        if (rc != Sqlite.DONE && rc != Sqlite.ROW) throw error(rc);
    }

    void feed(Out out) {
        Out b = feedBody;
        b.pos = 0;
        b.put(FEED_OPEN);
        try {
            int rc;
            while ((rc = Sqlite.step(feed)) == Sqlite.ROW) {
                if (b.pos > FEED_OPEN.length) b.put(COMMA);
                b.copyNative(Sqlite.columnText(feed, 0), Sqlite.columnBytes(feed, 0));
            }
            if (rc != Sqlite.DONE) throw error(rc);
        } finally {
            Sqlite.reset(feed);
        }
        b.put(FEED_CLOSE);
        out.head(Out.S200, b.pos);
        out.copyNative(b.seg, b.pos);
    }

    boolean post(long id, Out out) {
        Sqlite.bindInt64(post, 1, id);
        return jsonRow(post, out, Out.S200);
    }

    void insertPost(long userId, byte[] body, byte[] username, Out out) {
        begin();
        MemorySegment.copy(body, 0, scratch, JAVA_BYTE, 0, body.length);
        MemorySegment.copy(username, 0, scratch, JAVA_BYTE, body.length, username.length);
        Sqlite.bindInt64(insertPost, 1, userId);
        Sqlite.bindText(insertPost, 2, scratch, body.length);
        Sqlite.bindText(insertPost, 3, scratch.asSlice(body.length), username.length);
        try {
            int rc = Sqlite.step(insertPost);
            if (rc != Sqlite.ROW) throw error(rc);
            MemorySegment p = Sqlite.columnText(insertPost, 0);
            int n = Sqlite.columnBytes(insertPost, 0);
            out.head(Out.S201, n);
            out.copyNative(p, n);
            rc = Sqlite.step(insertPost);
            if (rc != Sqlite.DONE) throw error(rc);
        } finally {
            Sqlite.reset(insertPost);
        }
    }

    int like(long userId, long postId) {
        begin();
        Sqlite.bindInt64(like, 1, userId);
        Sqlite.bindInt64(like, 2, postId);
        run(like);
        if (Sqlite.changes(db) == 1) return 201;
        Sqlite.bindInt64(exists, 1, postId);
        int rc;
        try {
            rc = Sqlite.step(exists);
        } finally {
            Sqlite.reset(exists);
        }
        if (rc == Sqlite.ROW) return 200;
        if (rc == Sqlite.DONE) return 404;
        throw error(rc);
    }

    String ping() {
        try {
            int rc;
            try {
                rc = Sqlite.step(ping);
            } finally {
                Sqlite.reset(ping);
            }
            return rc == Sqlite.ROW ? null : Sqlite.errmsg(db);
        } catch (RuntimeException e) {
            return String.valueOf(e.getMessage());
        }
    }

    private void begin() {
        if (!inTxn) {
            run(begin);
            inTxn = true;
        }
    }

    boolean commit() {
        if (!inTxn) return true;
        inTxn = false;
        try {
            run(commit);
            unchecked++;
            return true;
        } catch (RuntimeException e) {
            System.err.println("commit failed: " + e.getMessage());
            try { run(rollback); } catch (RuntimeException ignored) { }
            return false;
        }
    }

    void checkpointIfDirty() {
        if (unchecked == 0) return;
        unchecked = 0;
        int rc = Sqlite.walCheckpointPassive(db);
        if (rc != Sqlite.OK) System.err.println("checkpoint failed: " + Sqlite.errmsg(db));
    }
}
