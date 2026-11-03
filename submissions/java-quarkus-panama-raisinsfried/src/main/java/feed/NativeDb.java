package feed;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.nio.charset.StandardCharsets.UTF_8;

import feed.Model.Post;
import feed.Sqlite.SqliteException;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.ArrayList;
import java.util.List;

/** One SQLite connection with its statements prepared once; used by one thread at a time. */
final class NativeDb implements AutoCloseable {
    private static final String POST_SELECT = """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
              FROM posts p JOIN users u ON u.id = p.user_id
            """;
    private static final String[] SQL = {
            "SELECT 1",
            POST_SELECT + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
            POST_SELECT + " WHERE p.id = ?",
            "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at",
            "SELECT 1 FROM posts WHERE id = ?",
            "INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING",
            "BEGIN IMMEDIATE", "COMMIT", "ROLLBACK"};
    private static final int PING = 0, FEED = 1, POST_BY_ID = 2, INSERT_POST = 3, POST_EXISTS = 4, INSERT_LIKE = 5,
            BEGIN = 6, COMMIT = 7, ROLLBACK = 8;

    /** mmap off: with mmap, SQLite looks every page fetch up in the WAL index first, even pages it has cached. */
    private static final List<String> PRAGMAS = List.of(
            "PRAGMA journal_mode = WAL",
            "PRAGMA synchronous = FULL",
            "PRAGMA busy_timeout = 5000",
            "PRAGMA cache_size = -16384", // KiB: 16 MiB
            "PRAGMA mmap_size = 0",
            "PRAGMA temp_store = MEMORY");

    record Inserted(long id, String createdAt) {}

    enum Like { CREATED, ALREADY_LIKED, NO_SUCH_POST }

    private final MemorySegment db;
    private final MemorySegment[] statements = new MemorySegment[SQL.length];

    NativeDb(String path) throws Throwable {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = arena.allocate(ADDRESS);
            // unix-excl: one process-wide lock on the file and the WAL index in heap memory, so read transactions
            // make no fcntl calls; other processes can't open the database while we run.
            int rc = (int) Sqlite.open_v2.invokeExact(arena.allocateFrom(path), out,
                    Sqlite.OPEN_READWRITE | Sqlite.OPEN_NOMUTEX | Sqlite.OPEN_EXRESCODE, arena.allocateFrom("unix-excl"));
            db = out.get(ADDRESS, 0);
            if (rc != Sqlite.OK) throw new SqliteException(rc, "cannot open " + path + ": " + errmsg());
            for (String pragma : PRAGMAS) {
                rc = (int) Sqlite.exec.invokeExact(db, arena.allocateFrom(pragma), MemorySegment.NULL,
                        MemorySegment.NULL, MemorySegment.NULL);
                if (rc != Sqlite.OK) throw error(rc);
            }
            for (int i = 0; i < SQL.length; i++) {
                MemorySegment sql = arena.allocateFrom(SQL[i]);
                rc = (int) Sqlite.prepare_v3.invokeExact(db, sql, (int) sql.byteSize(), Sqlite.PREPARE_PERSISTENT, out,
                        MemorySegment.NULL);
                if (rc != Sqlite.OK) throw error(rc);
                statements[i] = out.get(ADDRESS, 0);
            }
        }
    }

    void ping() throws Throwable {
        MemorySegment st = statements[PING];
        try {
            int rc = (int) Sqlite.step.invokeExact(st);
            if (rc != Sqlite.ROW) throw error(rc);
        } finally {
            reset(st);
        }
    }

    List<Post> feed() throws Throwable {
        MemorySegment st = statements[FEED];
        try {
            var posts = new ArrayList<Post>(20);
            for (int rc; (rc = (int) Sqlite.step.invokeExact(st)) != Sqlite.DONE; ) {
                if (rc != Sqlite.ROW) throw error(rc);
                posts.add(post(st));
            }
            return posts;
        } finally {
            reset(st);
        }
    }

    Post post(long id) throws Throwable {
        MemorySegment st = statements[POST_BY_ID];
        try {
            bind(st, 1, id);
            int rc = (int) Sqlite.step.invokeExact(st);
            if (rc == Sqlite.DONE) return null;
            if (rc != Sqlite.ROW) throw error(rc);
            return post(st);
        } finally {
            reset(st);
        }
    }

    Inserted insertPost(long userId, String body) throws Throwable {
        MemorySegment st = statements[INSERT_POST];
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment text = arena.allocateFrom(body); // UTF-8, NUL-terminated
            bind(st, 1, userId);
            int rc = (int) Sqlite.bind_text.invokeExact(st, 2, text, (int) text.byteSize() - 1, Sqlite.TRANSIENT);
            if (rc != Sqlite.OK) throw error(rc);
            if ((rc = (int) Sqlite.step.invokeExact(st)) != Sqlite.ROW) throw error(rc);
            var inserted = new Inserted((long) Sqlite.column_int64.invokeExact(st, 0), text(st, 1));
            if ((rc = (int) Sqlite.step.invokeExact(st)) != Sqlite.DONE) throw error(rc);
            return inserted;
        } finally {
            reset(st);
        }
    }

    /** The changed-row count tells a new like from a repeat. */
    Like like(long userId, long postId) throws Throwable {
        MemorySegment exists = statements[POST_EXISTS];
        try {
            bind(exists, 1, postId);
            int rc = (int) Sqlite.step.invokeExact(exists);
            if (rc == Sqlite.DONE) return Like.NO_SUCH_POST;
            if (rc != Sqlite.ROW) throw error(rc);
        } finally {
            reset(exists);
        }
        MemorySegment insert = statements[INSERT_LIKE];
        try {
            bind(insert, 1, userId);
            bind(insert, 2, postId);
            int rc = (int) Sqlite.step.invokeExact(insert);
            if (rc != Sqlite.DONE) throw error(rc);
            return (int) Sqlite.changes.invokeExact(db) == 1 ? Like.CREATED : Like.ALREADY_LIKED;
        } finally {
            reset(insert);
        }
    }

    void begin() throws Throwable {
        run(BEGIN);
    }

    void commit() throws Throwable {
        run(COMMIT);
    }

    void rollback() throws Throwable {
        if (!autocommit()) run(ROLLBACK);
    }

    /** SQLite rolls the transaction back by itself on some errors (IOERR, FULL, NOMEM). */
    boolean autocommit() throws Throwable {
        return (int) Sqlite.get_autocommit.invokeExact(db) != 0;
    }

    private void run(int statement) throws Throwable {
        MemorySegment st = statements[statement];
        try {
            int rc = (int) Sqlite.step.invokeExact(st);
            if (rc != Sqlite.DONE) throw error(rc);
        } finally {
            reset(st);
        }
    }

    private static Post post(MemorySegment st) throws Throwable {
        return new Post((long) Sqlite.column_int64.invokeExact(st, 0), text(st, 1), text(st, 2), text(st, 3),
                (long) Sqlite.column_int64.invokeExact(st, 4));
    }

    private static String text(MemorySegment st, int col) throws Throwable {
        long p = (long) Sqlite.column_text.invokeExact(st, col); // text first, then bytes (SQLite's rule)
        int n = (int) Sqlite.column_bytes.invokeExact(st, col);
        return p == 0 ? null : MemorySegment.ofAddress(p).reinterpret(n).getString(0, UTF_8, n);
    }

    private void bind(MemorySegment st, int index, long value) throws Throwable {
        int rc = (int) Sqlite.bind_int64.invokeExact(st, index, value);
        if (rc != Sqlite.OK) throw error(rc);
    }

    private static void reset(MemorySegment st) throws Throwable {
        int ignored = (int) Sqlite.reset.invokeExact(st); // repeats the last step's error, already handled
    }

    private SqliteException error(int rc) throws Throwable {
        return new SqliteException(rc, errmsg());
    }

    private String errmsg() throws Throwable {
        return Sqlite.string((MemorySegment) Sqlite.errmsg.invokeExact(db));
    }

    @Override
    public void close() {
        try {
            for (MemorySegment st : statements) {
                if (st != null) {
                    int ignored = (int) Sqlite.finalize_.invokeExact(st);
                }
            }
            int ignored = (int) Sqlite.close_v2.invokeExact(db);
        } catch (Throwable t) {
            throw Sqlite.rethrow(t);
        }
    }
}
