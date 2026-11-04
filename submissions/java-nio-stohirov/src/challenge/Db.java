package challenge;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;

/**
 * The event loop's database access. Every pass is one transaction: reads share its snapshot, writes are
 * group-committed, and no response is sent until {@link #commit} returns (rule 6). Reads go through
 * SQLite's B-tree cursors in csrc/shim.c; writes and the fallback reads are ordinary SQL.
 */
final class Db {
    private static final int USE_SQL = -2;

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

    private static final String LIKE_SQL =
            "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING";

    private final MemorySegment db;
    private final MemorySegment feedSql, postSql, insertPost, like, ping;
    private final MemorySegment scratch = Arena.global().allocate(64 * 1024);
    private final Out sqlBody = new Out(256 * 1024);
    private final Checkpointer checkpointer;
    private final long connection;

    private boolean inTxn;
    private int feedDigits = 4, postDigits = 3;

    Db(String path) {
        db = Sqlite.open(path);
        connection = db.address();
        feedSql = prepare(FEED_SQL);
        postSql = prepare(POST_SQL);
        insertPost = prepare(INSERT_POST_SQL);
        like = prepare(LIKE_SQL);
        ping = prepare("SELECT 1");
        checkpointer = new Checkpointer(Sqlite.checkpointConnection());
    }

    private MemorySegment prepare(String sql) {
        try (Arena a = Arena.ofConfined()) {
            MemorySegment pp = a.allocate(ADDRESS);
            if (Sqlite.prepareV3(db, a.allocateFrom(sql), Sqlite.PREPARE_PERSISTENT, pp) != Sqlite.OK) {
                throw new Sqlite.SqliteException("prepare " + sql + ": " + Sqlite.errmsg(db));
            }
            return pp.get(ADDRESS, 0);
        }
    }

    private Sqlite.SqliteException error(int rc) {
        return new Sqlite.SqliteException("sqlite rc " + rc + ": " + Sqlite.errmsg(db));
    }

    /** Joins the pass's transaction. inTxn is set first: even a failure must reach commit(), to report it. */
    private void begin() {
        inTxn = true;
        if (Sqlite.begin() != Sqlite.OK) throw new Sqlite.SqliteException("transaction is not available");
    }

    /** Ends the pass's transaction. False if it was lost: nothing in this pass may be sent. */
    boolean commit() {
        if (!inTxn) return true;
        inTxn = false;
        if (Sqlite.commit() == Sqlite.OK) return true;
        System.err.println("commit failed: " + Sqlite.errmsg(db));
        return false;
    }

    /** Between passes, after the commit. */
    void tick() { checkpointer.tick(connection); }

    /** Writes must wait: the checkpoint thread is copying the log's last frames. */
    boolean writesHeld() { return checkpointer.writesHeld(); }

    void feed(Out out) {
        begin();
        int mark = out.pos;
        int body = out.headForBody(Out.S200, feedDigits);
        int n = Sqlite.feed(out.address(body), out.capacity() - body);
        if (n >= 0) {
            feedDigits = out.endBody(body, n, feedDigits);
            return;
        }
        if (n != USE_SQL) throw new Sqlite.SqliteException("feed read failed: " + Sqlite.errmsg(db));
        out.pos = mark;
        feedWithSql(out);
    }

    private void feedWithSql(Out out) {
        Out b = sqlBody;
        b.pos = 0;
        b.put(FEED_OPEN);
        try {
            int rc;
            while ((rc = Sqlite.step(feedSql)) == Sqlite.ROW) {
                if (b.pos > FEED_OPEN.length) b.put(COMMA);
                b.copyNative(Sqlite.columnText(feedSql, 0), Sqlite.columnBytes(feedSql, 0));
            }
            if (rc != Sqlite.DONE) throw error(rc);
        } finally {
            Sqlite.reset(feedSql);
        }
        b.put(FEED_CLOSE);
        out.head(Out.S200, b.pos);
        out.copyNative(b.seg, b.pos);
    }

    boolean post(long id, Out out) {
        begin();
        int mark = out.pos;
        int body = out.headForBody(Out.S200, postDigits);
        int n = Sqlite.post(id, out.address(body), out.capacity() - body);
        if (n > 0) {
            postDigits = out.endBody(body, n, postDigits);
            return true;
        }
        out.pos = mark;
        if (n == 0) return false;
        if (n != USE_SQL) throw new Sqlite.SqliteException("post read failed: " + Sqlite.errmsg(db));
        Sqlite.bindInt64(postSql, 1, id);
        try {
            int rc = Sqlite.step(postSql);
            if (rc == Sqlite.DONE) return false;
            if (rc != Sqlite.ROW) throw error(rc);
            int len = Sqlite.columnBytes(postSql, 0);
            MemorySegment p = Sqlite.columnText(postSql, 0);
            out.head(Out.S200, len);
            out.copyNative(p, len);
            return true;
        } finally {
            Sqlite.reset(postSql);
        }
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

    /** 201 for a new like, 200 for a repeat, 404 if there is no such post. */
    int like(long userId, long postId) {
        begin();
        int exists = Sqlite.postExists(postId);
        if (exists < 0) throw new Sqlite.SqliteException("post lookup failed: " + Sqlite.errmsg(db));
        if (exists == 0) return 404;
        Sqlite.bindInt64(like, 1, userId);
        Sqlite.bindInt64(like, 2, postId);
        int rc;
        try {
            rc = Sqlite.step(like);
        } finally {
            Sqlite.reset(like);
        }
        if (rc != Sqlite.DONE) throw error(rc);
        return Sqlite.changes(db) == 1 ? 201 : 200;
    }

    String ping() {
        try {
            begin();
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
}
