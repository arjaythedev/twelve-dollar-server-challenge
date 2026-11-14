package challenge;

import io.netty.buffer.ByteBuf;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;

/**
 * The serving connection and its prepared statements. Rows are written straight from SQLite's
 * memory into the response buffer as JSON, without making Strings.
 *
 * Not thread-safe: used only from the event loop thread.
 */
final class Db {
    private static final String POST_COLUMNS =
            "SELECT p.id, p.body, p.created_at, u.username,"
            + " (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"
            + " FROM posts p JOIN users u ON u.id = p.user_id";

    private final MemorySegment db;
    private final MemorySegment feed, post, insertPost, like, postExists, select1, begin, commit, rollback;
    private final Arena arena = Arena.ofAuto();
    private final MemorySegment textArg = arena.allocate(4096);
    private final byte[] scratch = new byte[4096];
    private boolean inTransaction;

    Db(String path) {
        db = Sqlite.open(path);
        Sqlite.exec(db, "PRAGMA busy_timeout = 5000");
        Sqlite.exec(db, "PRAGMA journal_mode = WAL");
        Sqlite.exec(db, "PRAGMA synchronous = NORMAL");
        // checkpoints run on their own thread and connection (see Checkpointer)
        Sqlite.exec(db, "PRAGMA wal_autocheckpoint = 0");
        Sqlite.exec(db, "PRAGMA mmap_size = 1073741824");
        Sqlite.exec(db, "PRAGMA cache_size = -32768");
        Sqlite.exec(db, "PRAGMA temp_store = MEMORY");

        feed = Sqlite.prepare(db, POST_COLUMNS + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
        post = Sqlite.prepare(db, POST_COLUMNS + " WHERE p.id = ?1");
        insertPost = Sqlite.prepare(db, "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
        like = Sqlite.prepare(db,
                "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)"
                + " ON CONFLICT (user_id, post_id) DO NOTHING");
        postExists = Sqlite.prepare(db, "SELECT 1 FROM posts WHERE id = ?1");
        select1 = Sqlite.prepare(db, "SELECT 1");
        begin = Sqlite.prepare(db, "BEGIN IMMEDIATE");
        commit = Sqlite.prepare(db, "COMMIT");
        rollback = Sqlite.prepare(db, "ROLLBACK");
    }

    void health() {
        try {
            Sqlite.step(db, select1);
        } finally {
            Sqlite.reset(select1);
        }
    }

    /** {"posts":[...20 newest...]} */
    void writeFeed(ByteBuf out) {
        try {
            out.writeBytes(FEED_OPEN);
            boolean first = true;
            while (Sqlite.step(db, feed) == Sqlite.ROW) {
                if (!first) out.writeByte(',');
                first = false;
                writePost(feed, out);
            }
            out.writeBytes(FEED_CLOSE);
        } finally {
            Sqlite.reset(feed);
        }
    }

    /** {"post":{...}}, or false (nothing written) if there is no such post. */
    boolean writePostById(long id, ByteBuf out) {
        try {
            Sqlite.bindLong(post, 1, id);
            if (Sqlite.step(db, post) != Sqlite.ROW) return false;
            out.writeBytes(POST_OPEN);
            writePost(post, out);
            out.writeByte('}');
            return true;
        } finally {
            Sqlite.reset(post);
        }
    }

    /** Inserts a post (inside the open transaction) and writes {"post":{...}}. body is trimmed UTF-8. */
    void createPost(long userId, byte[] body, String username, ByteBuf out) {
        MemorySegment text = body.length <= textArg.byteSize() ? textArg : arena.allocate(body.length);
        MemorySegment.copy(body, 0, text, JAVA_BYTE, 0, body.length);
        try {
            Sqlite.bindLong(insertPost, 1, userId);
            Sqlite.bindText(insertPost, 2, text, body.length);
            if (Sqlite.step(db, insertPost) != Sqlite.ROW) throw new Sqlite.SqliteException("insert returned no row");
            out.writeBytes(POST_OPEN).writeBytes(ID_OPEN);
            Json.writeLong(out, Sqlite.columnLong(insertPost, 0));
            out.writeBytes(BODY);
            Json.writeEscaped(out, body, 0, body.length);
            out.writeBytes(CREATED_AT);
            writeText(insertPost, 1, out);
            out.writeBytes(AUTHOR);
            Json.writeEscaped(out, username);
            out.writeBytes(NEW_POST_CLOSE);
            while (Sqlite.step(db, insertPost) == Sqlite.ROW) { /* drain to DONE */ }
        } finally {
            Sqlite.reset(insertPost);
        }
    }

    static final int LIKED = 201, ALREADY_LIKED = 200, NO_SUCH_POST = 404;

    /** Likes a post (inside the open transaction). */
    int like(long userId, long postId) {
        try {
            Sqlite.bindLong(like, 1, userId);
            Sqlite.bindLong(like, 2, postId);
            Sqlite.step(db, like);
        } finally {
            Sqlite.reset(like);
        }
        if (Sqlite.changes(db) == 1) return LIKED;
        try {
            Sqlite.bindLong(postExists, 1, postId);
            return Sqlite.step(db, postExists) == Sqlite.ROW ? ALREADY_LIKED : NO_SUCH_POST;
        } finally {
            Sqlite.reset(postExists);
        }
    }

    boolean inTransaction() { return inTransaction; }

    void begin() {
        run(begin);
        inTransaction = true;
    }

    void commit() {
        try {
            run(commit);
        } catch (RuntimeException e) {
            try { run(rollback); } catch (RuntimeException ignored) { }
            throw e;
        } finally {
            inTransaction = false;
        }
    }

    private void run(MemorySegment stmt) {
        try {
            Sqlite.step(db, stmt);
        } finally {
            Sqlite.reset(stmt);
        }
    }

    // {"id":1,"body":"...","created_at":"...","author":"...","like_count":11}
    private void writePost(MemorySegment stmt, ByteBuf out) {
        out.writeBytes(ID_OPEN);
        Json.writeLong(out, Sqlite.columnLong(stmt, 0));
        out.writeBytes(BODY);
        writeText(stmt, 1, out);
        out.writeBytes(CREATED_AT);
        writeText(stmt, 2, out);
        out.writeBytes(AUTHOR);
        writeText(stmt, 3, out);
        out.writeBytes(LIKE_COUNT);
        Json.writeLong(out, Sqlite.columnLong(stmt, 4));
        out.writeByte('}');
    }

    private void writeText(MemorySegment stmt, int col, ByteBuf out) {
        MemorySegment text = Sqlite.columnText(stmt, col);
        int len = (int) text.byteSize();
        byte[] buf = len <= scratch.length ? scratch : new byte[len];
        MemorySegment.copy(text, JAVA_BYTE, 0, buf, 0, len);
        Json.writeEscaped(out, buf, 0, len);
    }

    private static byte[] ascii(String s) { return s.getBytes(StandardCharsets.US_ASCII); }

    private static final byte[] FEED_OPEN = ascii("{\"posts\":[");
    private static final byte[] FEED_CLOSE = ascii("]}");
    private static final byte[] POST_OPEN = ascii("{\"post\":");
    private static final byte[] ID_OPEN = ascii("{\"id\":");
    private static final byte[] BODY = ascii(",\"body\":\"");
    private static final byte[] CREATED_AT = ascii("\",\"created_at\":\"");
    private static final byte[] AUTHOR = ascii("\",\"author\":\"");
    private static final byte[] LIKE_COUNT = ascii("\",\"like_count\":");
    private static final byte[] NEW_POST_CLOSE = ascii("\",\"like_count\":0}}");
}
