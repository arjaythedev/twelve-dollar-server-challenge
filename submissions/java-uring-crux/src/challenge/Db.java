package challenge;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;

/**
 * One SQLite connection. Callers hold one deferred transaction open across read-only batches
 * so the pager is set up once, and {@link #commit()} before any write response is sent.
 */
final class Db {
    private static final String PRAGMAS = """
            PRAGMA locking_mode = EXCLUSIVE;
            PRAGMA journal_mode = WAL;
            PRAGMA synchronous = NORMAL;
            PRAGMA foreign_keys = ON;
            PRAGMA cache_size = 500;
            PRAGMA temp_store = MEMORY;
            PRAGMA mmap_size = 1073741824;
            PRAGMA journal_size_limit = 67108864;
            PRAGMA wal_autocheckpoint = 0;
            """;

    private static final String POST_COLS = """
            json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username,
                        'like_count',(SELECT count(*) FROM likes l WHERE l.post_id = p.id))""";

    private static final byte[] FEED_OPEN = Out.ascii("{\"posts\":[");
    private static final byte[] FEED_CLOSE = Out.ascii("]}");
    private static final byte[] COMMA = { ',' };
    /** Matches {@code crux_post_json}: the object does not fit the destination. */
    private static final int JSON_OVERFLOW = -1_000_000;

    private final MemorySegment db;
    private final MemorySegment feedFallback, feedIds, feedLikes, feedCols, post, insertPost, like, exists, ping;
    private final MemorySegment begin, commit, rollback;
    private final MemorySegment scratch = Arena.global().allocate(8 * 1024);
    /** Off-heap JSON when the caller is on the heap. A normal downcall cannot take a heap array. */
    private final MemorySegment fieldsN = Arena.global().allocate(128 * 1024L, 8);

    private boolean open;
    private int writesSinceCheckpoint;
    private long lastCheckpoint;

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
        feedFallback = prepare("SELECT " + POST_COLS
                + " FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
        feedIds = prepare("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20");
        feedLikes = prepare("SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2");
        feedCols = prepare("""
                SELECT p.id, p.body, p.created_at, u.username
                  FROM posts p JOIN users u ON u.id = p.user_id
                 WHERE p.id BETWEEN ?1 AND ?2""");
        post = prepare("""
                SELECT p.body, p.created_at, u.username,
                       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
                  FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1""");
        insertPost = prepare("""
                INSERT INTO posts (user_id, body) VALUES (?1, ?2)
                RETURNING json_object('post', json_object('id',id,'body',body,'created_at',created_at,
                          'author',?3,'like_count',0))""");
        like = prepare("""
                INSERT INTO likes (user_id, post_id)
                SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)
                ON CONFLICT (user_id, post_id) DO NOTHING""");
        exists = prepare("SELECT 1 FROM posts WHERE id = ?1");
        ping = prepare("SELECT 1");
        begin = prepare("BEGIN");
        commit = prepare("COMMIT");
        rollback = prepare("ROLLBACK");
    }

    private void check(int rc, String what) {
        if (rc != Sqlite.OK) throw new Sqlite.SqliteException(what + ": " + (db == null ? "rc " + rc : Sqlite.errmsg(db)));
    }

    private MemorySegment prepare(String sql) {
        try (Arena a = Arena.ofConfined()) {
            MemorySegment pp = a.allocate(ADDRESS);
            check(Sqlite.prepareV3(db, a.allocateFrom(sql), Sqlite.PREPARE_PERSISTENT, pp), "prepare");
            return pp.get(ADDRESS, 0);
        }
    }

    private Sqlite.SqliteException error(int rc) {
        return new Sqlite.SqliteException("sqlite rc " + rc + ": " + Sqlite.errmsg(db));
    }

    void begin() {
        if (open) return;
        run(begin);
        open = true;
    }

    /** @return false when the commit failed and the batch was rolled back */
    boolean commit() { return commit(true); }

    /**
     * @param wrote whether this batch changed the database. Read-only commits do not arm a checkpoint.
     * @return false when the commit failed and the batch was rolled back
     */
    boolean commit(boolean wrote) {
        if (!open) return true;
        open = false;
        try {
            run(commit);
            if (wrote) writesSinceCheckpoint++;
            return true;
        } catch (RuntimeException e) {
            System.err.println("commit failed: " + e.getMessage());
            try { run(rollback); } catch (RuntimeException ignored) { }
            return false;
        }
    }

    void rollbackQuietly() {
        if (!open) return;
        open = false;
        try { run(rollback); } catch (RuntimeException ignored) { }
    }

    /** RESTART checkpoint after 4096 commits. sqlite3_wal_checkpoint_v2 mode 2. */
    void checkpointIfDue() {
        if (writesSinceCheckpoint < 4096) return;
        long now = System.nanoTime();
        if (now - lastCheckpoint < 50_000_000L) return;
        // A long-lived read snapshot blocks the restart checkpoint.
        if (open && !commit(false)) return;
        lastCheckpoint = now;
        int rc = Sqlite.walCheckpoint(db, 2);
        if (rc == Sqlite.OK) writesSinceCheckpoint = 0;
        else if (rc != 5) System.err.println("checkpoint failed: " + Sqlite.errmsg(db));
    }

    String ping() {
        try {
            int rc = Sqlite.step(ping);
            Sqlite.reset(ping);
            return rc == Sqlite.ROW ? null : Sqlite.errmsg(db);
        } catch (RuntimeException e) {
            return String.valueOf(e.getMessage());
        }
    }

    /**
     * Appends the feed JSON object. The narrow window is formatted by the SQLite library
     * straight into {@code out} when that buffer is native. A wide id span, or a buffer
     * that cannot hold the object, uses the join.
     */
    void feed(Out out) {
        boolean direct = out.direct() && out.room() > 0;
        MemorySegment dst = direct ? out.remaining() : fieldsN;
        int n;
        try {
            n = Sqlite.feedJson(feedIds, feedLikes, feedCols, dst, (int) dst.byteSize());
        } finally {
            Sqlite.reset(feedIds);
            Sqlite.reset(feedLikes);
            Sqlite.reset(feedCols);
        }
        if (n == 0) {
            fallbackFeed(out);
            return;
        }
        if (n < 0) throw error(-n);
        if (direct) out.advance(n);
        else out.putSegment(dst.asSlice(0, n), n);
    }

    private void fallbackFeed(Out out) {
        out.put(FEED_OPEN);
        boolean any = false;
        int rc;
        try {
            while ((rc = Sqlite.step(feedFallback)) == Sqlite.ROW) {
                if (any) out.put(COMMA);
                any = true;
                Sqlite.appendText(feedFallback, 0, out);
            }
        } finally {
            Sqlite.reset(feedFallback);
        }
        if (rc != Sqlite.DONE) throw error(rc);
        out.put(FEED_CLOSE);
    }

    boolean post(long id, Out out) {
        boolean direct = out.direct() && out.room() >= 1024;
        MemorySegment dst = direct ? out.remaining() : fieldsN;
        try {
            int n = Sqlite.postJson(post, id, dst, (int) dst.byteSize());
            if (n == JSON_OVERFLOW) {
                if (!direct) throw error(Sqlite.OK);
                throw new IndexOutOfBoundsException("post");
            }
            if (n < 0) throw error(-n);
            if (n == 0) return false;
            if (direct) out.advance(n);
            else out.putSegment(dst.asSlice(0, n), n);
            return true;
        } finally {
            Sqlite.reset(post);
        }
    }

    void insertPost(long userId, byte[] body, byte[] username, Out out) {
        if (body.length + username.length > scratch.byteSize()) {
            throw new Sqlite.SqliteException("post body does not fit the scratch buffer");
        }
        MemorySegment.copy(body, 0, scratch, JAVA_BYTE, 0, body.length);
        MemorySegment.copy(username, 0, scratch, JAVA_BYTE, body.length, username.length);
        Sqlite.bindInt64(insertPost, 1, userId);
        Sqlite.bindText(insertPost, 2, scratch, body.length);
        Sqlite.bindText(insertPost, 3, scratch.asSlice(body.length), username.length);
        try {
            int rc = Sqlite.step(insertPost);
            if (rc != Sqlite.ROW) throw error(rc);
            Sqlite.appendText(insertPost, 0, out);
            rc = Sqlite.step(insertPost);
            if (rc != Sqlite.DONE) throw error(rc);
        } finally {
            Sqlite.reset(insertPost);
        }
    }

    /** @return 201 first like, 200 repeat, 404 missing post */
    int like(long userId, long postId) {
        Sqlite.bindInt64(like, 1, userId);
        Sqlite.bindInt64(like, 2, postId);
        run(like);
        if (Sqlite.changes(db) == 1) return 201;
        Sqlite.bindInt64(exists, 1, postId);
        try {
            int rc = Sqlite.step(exists);
            if (rc == Sqlite.ROW) return 200;
            if (rc == Sqlite.DONE) return 404;
            throw error(rc);
        } finally {
            Sqlite.reset(exists);
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
}
