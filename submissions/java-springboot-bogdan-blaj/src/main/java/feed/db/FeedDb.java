package feed.db;

import io.netty.util.concurrent.EventExecutor;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;

import static feed.db.Sqlite.SQLITE_CHECKPOINT_RESTART;
import static feed.db.Sqlite.SQLITE_DONE;
import static feed.db.Sqlite.SQLITE_OK;
import static feed.db.Sqlite.SQLITE_OPEN_NOMUTEX;
import static feed.db.Sqlite.SQLITE_OPEN_READWRITE;
import static feed.db.Sqlite.SQLITE_PREPARE_PERSISTENT;
import static feed.db.Sqlite.SQLITE_ROW;
import static feed.db.Sqlite.SQLITE_STATIC;
import static java.lang.foreign.MemorySegment.NULL;
import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;

/**
 * The single SQLite connection. Every public method must run on the event-loop thread that owns it (the library
 * is built with {@code SQLITE_THREADSAFE=0}) and checks this.
 *
 * <p>Statements are prepared once in {@link #open}. Response bodies are built by SQLite and handed to a
 * {@link BytesSink} (or, for the feed, row by row to a {@link PartsSink}) straight from {@code sqlite3_column_blob};
 * the pointer is only valid during the call.
 *
 * <p>Errors: any unexpected result code throws {@link DbException}. A constraint failure rolls back only the
 * failing statement; an open transaction stays open. After any other error inside a transaction, check
 * {@link #autocommit()}: true means SQLite rolled the whole transaction back.
 */
public final class FeedDb {

    private static final Logger log = LoggerFactory.getLogger(FeedDb.class);
    private static final int OPEN_FLAGS = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX;

    public static final int LIKE_CREATED = 1;
    public static final int LIKE_REPEAT = 2;
    public static final int LIKE_NO_POST = 3;

    /** Each of the two text parameters of create gets half of the scratch buffer. */
    static final int SCRATCH_TEXT_BYTES = 32 * 1024;
    private static final int PREWARM_READ_BYTES = 1 << 20;

    private static final BytesSink DISCARD = (src, len) -> {
    };
    private static final PartsSink DISCARD_PARTS = new PartsSink() {
        @Override
        public void begin() {
        }

        @Override
        public void append(MemorySegment src, int len) {
        }

        @Override
        public void end() {
        }
    };

    private static final byte[] FEED_OPEN = "{\"posts\":[".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] FEED_CLOSE = "]}".getBytes(StandardCharsets.US_ASCII);

    private final EventExecutor loop;

    // Long-lived native memory: Arena.global() segments carry no owner-thread or liveness checks on the hot path.
    private final MemorySegment bodyText;
    private final MemorySegment usernameText;
    private final MemorySegment ppOut;
    private final MemorySegment pnLog;
    private final MemorySegment pnCkpt;
    private final MemorySegment mainSchema;
    private final MemorySegment walHookStub;
    private final MemorySegment feedOpen;
    private final MemorySegment feedComma;
    private final MemorySegment feedClose;

    private MemorySegment db = NULL;
    private MemorySegment feedStmt = NULL;
    private MemorySegment postStmt = NULL;
    private MemorySegment createStmt = NULL;
    private MemorySegment likeStmt = NULL;
    private MemorySegment postExistsStmt = NULL;
    private MemorySegment healthStmt = NULL;
    private MemorySegment beginStmt = NULL;
    private MemorySegment commitStmt = NULL;
    private MemorySegment rollbackStmt = NULL;

    private int walFrames;
    private int lastCheckpointLog;
    private int lastCheckpointCkpt;
    private String version;
    private long prewarmNanos;
    private Map<String, String> queryPlans = Map.of();

    public FeedDb(EventExecutor loop) {
        this.loop = loop;
        Arena global = Arena.global();
        MemorySegment scratch = global.allocate(2L * SCRATCH_TEXT_BYTES, 16);
        bodyText = scratch.asSlice(0, SCRATCH_TEXT_BYTES);
        usernameText = scratch.asSlice(SCRATCH_TEXT_BYTES, SCRATCH_TEXT_BYTES);
        ppOut = global.allocate(ADDRESS);
        pnLog = global.allocate(JAVA_INT);
        pnCkpt = global.allocate(JAVA_INT);
        mainSchema = global.allocateFrom("main");
        walHookStub = Sqlite.upcall(walHookHandle(this), Sqlite.WAL_HOOK_DESCRIPTOR, global);
        feedOpen = constant(global, FEED_OPEN);
        feedComma = constant(global, new byte[]{','});
        feedClose = constant(global, FEED_CLOSE);
    }

    private static MemorySegment constant(Arena arena, byte[] bytes) {
        MemorySegment m = arena.allocate(bytes.length);
        MemorySegment.copy(bytes, 0, m, JAVA_BYTE, 0, bytes.length);
        return m;
    }

    private static MethodHandle walHookHandle(FeedDb target) {
        try {
            return MethodHandles.lookup()
                    .findVirtual(FeedDb.class, "onWalCommit", MethodType.methodType(int.class,
                            MemorySegment.class, MemorySegment.class, MemorySegment.class, int.class))
                    .bindTo(target);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(e);
        }
    }

    /** wal_hook upcall, runs inside the COMMIT step. Must not throw: an exception in an upcall kills the JVM. */
    private int onWalCommit(MemorySegment arg, MemorySegment connection, MemorySegment schema, int nFrames) {
        walFrames = nFrames;
        return SQLITE_OK;
    }

    // ---- lifecycle

    /** Opens an existing database (never creates one), applies the pragmas, prepares, explains and prewarms. */
    public void open(Path path) {
        checkLoop();
        if (!db.equals(NULL)) {
            throw new IllegalStateException("database already open");
        }
        version = Sqlite.libversion();
        if (!Sqlite.EXPECTED_VERSION.equals(version)) {
            throw new IllegalStateException("loaded SQLite " + version + ", expected " + Sqlite.EXPECTED_VERSION);
        }
        long t0 = System.nanoTime();
        try (Arena tmp = Arena.ofConfined()) {
            int rc = Sqlite.open_v2(tmp.allocateFrom(path.toString()), ppOut, OPEN_FLAGS, NULL);
            MemorySegment handle = ppOut.get(ADDRESS, 0);
            if (rc != SQLITE_OK) {
                String msg = handle.equals(NULL) ? Sqlite.errstr(rc) : Sqlite.errmsg(handle);
                int ext = handle.equals(NULL) ? rc : Sqlite.extended_errcode(handle);
                if (!handle.equals(NULL)) {
                    Sqlite.close_v2(handle);
                }
                throw new DbException("open " + path, rc & 0xff, ext, msg);
            }
            db = handle;
        }
        try {
            // Must precede any other access: the WAL index then lives in heap memory and no -shm file exists.
            String locking = queryText("PRAGMA locking_mode = EXCLUSIVE");
            if (!"exclusive".equals(locking)) {
                throw new IllegalStateException("locking_mode is " + locking + ", expected exclusive");
            }
            String mode = queryText("PRAGMA journal_mode = WAL");
            if (!"wal".equals(mode)) {
                throw new IllegalStateException("journal_mode is " + mode + ", expected wal");
            }
            for (String pragma : Sql.PRAGMAS_AFTER_JOURNAL_MODE) {
                queryText(pragma);
            }
            // After wal_autocheckpoint = 0: that pragma replaces whatever wal_hook is registered.
            Sqlite.wal_hook(db, walHookStub, NULL);

            feedStmt = prepare(Sql.FEED);
            postStmt = prepare(Sql.POST);
            createStmt = prepare(Sql.CREATE);
            likeStmt = prepare(Sql.LIKE);
            postExistsStmt = prepare(Sql.POST_EXISTS);
            healthStmt = prepare(Sql.HEALTH);
            beginStmt = prepare(Sql.BEGIN);
            commitStmt = prepare(Sql.COMMIT);
            rollbackStmt = prepare(Sql.ROLLBACK);

            Map<String, String> plans = new LinkedHashMap<>();
            plans.put("feed", explain(Sql.FEED));
            plans.put("post", explain(Sql.POST));
            plans.put("create", explain(Sql.CREATE));
            plans.put("like", explain(Sql.LIKE));
            plans.put("postExists", explain(Sql.POST_EXISTS));
            queryPlans = Collections.unmodifiableMap(plans);
            plans.forEach((name, plan) -> log.info("query plan {}:\n{}", name, plan));

            String err = health();
            if (err != null) {
                throw new IllegalStateException("SELECT 1 failed: " + err);
            }
            long opened = System.nanoTime();
            prewarm(path);
            prewarmNanos = System.nanoTime() - opened;
            log.info("opened {} (sqlite {}, wal, exclusive): setup {} ms, prewarm {} ms", path, version,
                    (opened - t0) / 1_000_000, prewarmNanos / 1_000_000);
        } catch (RuntimeException e) {
            close();
            throw e;
        }
    }

    /** Rolls back an open transaction, checkpoints the WAL (TRUNCATE), finalizes everything, closes. Idempotent. */
    public void close() {
        checkLoop();
        if (db.equals(NULL)) {
            return;
        }
        if (Sqlite.get_autocommit(db) == 0) {
            log.warn("closing with an open transaction; rolling it back");
            rollback();
        }
        feedStmt = finalizeStmt(feedStmt);
        postStmt = finalizeStmt(postStmt);
        createStmt = finalizeStmt(createStmt);
        likeStmt = finalizeStmt(likeStmt);
        postExistsStmt = finalizeStmt(postExistsStmt);
        healthStmt = finalizeStmt(healthStmt);
        beginStmt = finalizeStmt(beginStmt);
        commitStmt = finalizeStmt(commitStmt);
        rollbackStmt = finalizeStmt(rollbackStmt);
        try {
            queryText("PRAGMA wal_checkpoint(TRUNCATE)");
        } catch (RuntimeException e) {
            log.warn("final checkpoint failed: {}", e.getMessage());
        }
        int rc = Sqlite.close_v2(db);
        if (rc != SQLITE_OK) {
            log.warn("sqlite3_close_v2 returned {}", rc);
        }
        db = NULL;
        walFrames = 0;
    }

    public boolean isOpen() {
        return !db.equals(NULL);
    }

    public String libVersion() {
        return version;
    }

    // ---- requests

    /** Writes the whole {@code {"posts":[...]}} body to {@code sink}: the opening, the rows comma-separated, the close. */
    public void feed(PartsSink sink) {
        checkLoop();
        MemorySegment s = feedStmt;
        try {
            sink.begin();
            sink.append(feedOpen, FEED_OPEN.length);
            int rc;
            boolean first = true;
            while ((rc = Sqlite.step(s)) == SQLITE_ROW) {
                if (!first) {
                    sink.append(feedComma, 1);
                }
                first = false;
                MemorySegment p = Sqlite.column_blob(s, 0);
                int n = Sqlite.column_bytes(s, 0);
                sink.append(p.reinterpret(n), n);
            }
            if (rc != SQLITE_DONE) {
                throw error("feed", rc);
            }
            sink.append(feedClose, FEED_CLOSE.length);
            sink.end();
        } finally {
            Sqlite.reset(s);
        }
    }

    /** Calls {@code sink} with the {@code {"post":{...}}} body and returns true, or returns false if no such post. */
    public boolean post(long id, BytesSink sink) {
        checkLoop();
        MemorySegment s = postStmt;
        try {
            int rc = Sqlite.bind_int64(s, 1, id);
            if (rc != SQLITE_OK) {
                throw error("post bind", rc);
            }
            rc = Sqlite.step(s);
            if (rc == SQLITE_ROW) {
                emit(s, sink);
                return true;
            }
            if (rc == SQLITE_DONE) {
                return false;
            }
            throw error("post", rc);
        } finally {
            Sqlite.reset(s);
        }
    }

    /**
     * Inserts a post and calls {@code sink} with the {@code {"post":{...}}} body built by RETURNING.
     *
     * <p>The sink runs before the statement has finished: if this method then throws, whatever the sink produced
     * must be discarded. Only a normal return means the insert succeeded.
     *
     * @param body     UTF-8 bytes of the trimmed body, {@code bodyLen} of them used
     * @param username UTF-8 bytes of the author name for the response, {@code usernameLen} of them used
     */
    public void create(long userId, byte[] body, int bodyLen, byte[] username, int usernameLen, BytesSink sink) {
        checkLoop();
        if (bodyLen < 0 || bodyLen > SCRATCH_TEXT_BYTES || usernameLen < 0 || usernameLen > SCRATCH_TEXT_BYTES) {
            throw new IllegalArgumentException("create: text too long (body " + bodyLen + ", username "
                    + usernameLen + " bytes, max " + SCRATCH_TEXT_BYTES + ")");
        }
        MemorySegment.copy(body, 0, bodyText, JAVA_BYTE, 0, bodyLen);
        MemorySegment.copy(username, 0, usernameText, JAVA_BYTE, 0, usernameLen);
        MemorySegment s = createStmt;
        try {
            int rc = Sqlite.bind_int64(s, 1, userId);
            if (rc == SQLITE_OK) {
                rc = Sqlite.bind_text(s, 2, bodyText, bodyLen, SQLITE_STATIC);
            }
            if (rc == SQLITE_OK) {
                rc = Sqlite.bind_text(s, 3, usernameText, usernameLen, SQLITE_STATIC);
            }
            if (rc != SQLITE_OK) {
                throw error("create bind", rc);
            }
            rc = Sqlite.step(s);
            if (rc != SQLITE_ROW) {
                throw error("create", rc);
            }
            emit(s, sink);
            rc = Sqlite.step(s);
            if (rc != SQLITE_DONE) {
                throw error("create", rc);
            }
        } finally {
            Sqlite.reset(s);
        }
    }

    /** Returns {@link #LIKE_CREATED}, {@link #LIKE_REPEAT} or {@link #LIKE_NO_POST}. */
    public int like(long userId, long postId) {
        checkLoop();
        MemorySegment s = likeStmt;
        int changes;
        try {
            int rc = Sqlite.bind_int64(s, 1, userId);
            if (rc == SQLITE_OK) {
                rc = Sqlite.bind_int64(s, 2, postId);
            }
            if (rc != SQLITE_OK) {
                throw error("like bind", rc);
            }
            rc = Sqlite.step(s);
            if (rc != SQLITE_DONE) {
                throw error("like", rc);
            }
            changes = Sqlite.changes(db);
        } finally {
            Sqlite.reset(s);
        }
        if (changes > 0) {
            return LIKE_CREATED;
        }
        s = postExistsStmt;
        try {
            int rc = Sqlite.bind_int64(s, 1, postId);
            if (rc != SQLITE_OK) {
                throw error("like probe bind", rc);
            }
            rc = Sqlite.step(s);
            if (rc == SQLITE_ROW) {
                return LIKE_REPEAT;
            }
            if (rc == SQLITE_DONE) {
                return LIKE_NO_POST;
            }
            throw error("like probe", rc);
        } finally {
            Sqlite.reset(s);
        }
    }

    /** Runs {@code SELECT 1}. Returns null when it succeeds, otherwise the SQLite error message. */
    public String health() {
        checkLoop();
        if (db.equals(NULL)) {
            return "database is closed";
        }
        try {
            int rc = Sqlite.step(healthStmt);
            return rc == SQLITE_ROW ? null : Sqlite.errmsg(db);
        } finally {
            Sqlite.reset(healthStmt);
        }
    }

    // ---- transactions

    public void begin() {
        checkLoop();
        MemorySegment s = beginStmt;
        try {
            int rc = Sqlite.step(s);
            if (rc != SQLITE_DONE) {
                throw error("BEGIN", rc);
            }
        } finally {
            Sqlite.reset(s);
        }
    }

    /** {@code COMMIT}; returns the raw result code ({@code SQLITE_DONE} on success) and never throws for it. */
    public int commit() {
        checkLoop();
        MemorySegment s = commitStmt;
        try {
            return Sqlite.step(s);
        } finally {
            Sqlite.reset(s);
        }
    }

    /** {@code ROLLBACK} if a transaction is open; no-op in autocommit mode. Logs instead of throwing. */
    public void rollback() {
        checkLoop();
        if (db.equals(NULL) || Sqlite.get_autocommit(db) != 0) {
            return;
        }
        MemorySegment s = rollbackStmt;
        try {
            int rc = Sqlite.step(s);
            if (rc != SQLITE_DONE) {
                log.warn("ROLLBACK failed: {} (code {}, extended {})", Sqlite.errmsg(db), rc,
                        Sqlite.extended_errcode(db));
            }
        } finally {
            Sqlite.reset(s);
        }
    }

    /** True when no transaction is open ({@code sqlite3_get_autocommit != 0}); also true when closed. */
    public boolean autocommit() {
        checkLoop();
        return db.equals(NULL) || Sqlite.get_autocommit(db) != 0;
    }

    /** Most recent {@code sqlite3_errmsg} of the connection, for logging after a failed {@link #commit()}. */
    public String lastErrorMessage() {
        checkLoop();
        return db.equals(NULL) ? "database is closed" : Sqlite.errmsg(db);
    }

    public int lastExtendedErrcode() {
        checkLoop();
        return db.equals(NULL) ? Sqlite.SQLITE_MISUSE : Sqlite.extended_errcode(db);
    }

    // ---- checkpoints

    /** WAL size in frames as last reported by the wal_hook (during a COMMIT); 0 after {@link #checkpoint()}. */
    public int walFrames() {
        checkLoop();
        return walFrames;
    }

    /** RESTART checkpoint of the WAL; returns its duration in nanoseconds and resets {@link #walFrames()}. */
    public long checkpoint() {
        checkLoop();
        if (db.equals(NULL)) {
            throw new IllegalStateException("database is closed");
        }
        long t0 = System.nanoTime();
        int rc = Sqlite.wal_checkpoint_v2(db, mainSchema, SQLITE_CHECKPOINT_RESTART, pnLog, pnCkpt);
        long elapsed = System.nanoTime() - t0;
        if (rc != SQLITE_OK) {
            throw error("checkpoint", rc);
        }
        lastCheckpointLog = pnLog.get(JAVA_INT, 0);
        lastCheckpointCkpt = pnCkpt.get(JAVA_INT, 0);
        walFrames = 0;
        return elapsed;
    }

    // ---- package-private, for tests and diagnostics

    Map<String, String> queryPlans() {
        return queryPlans;
    }

    long prewarmNanos() {
        return prewarmNanos;
    }

    int lastCheckpointLog() {
        return lastCheckpointLog;
    }

    int lastCheckpointCkpt() {
        return lastCheckpointCkpt;
    }

    /** Runs a one-off statement on the loop and returns column 0 of its first row as text. */
    String queryTextForTest(String sql) {
        checkLoop();
        return queryText(sql);
    }

    // ---- helpers (event loop only)

    private void checkLoop() {
        if (!loop.inEventLoop()) {
            throw new IllegalStateException("SQLite called off the event loop from " + Thread.currentThread().getName());
        }
    }

    private static void emit(MemorySegment stmt, BytesSink sink) {
        MemorySegment p = Sqlite.column_blob(stmt, 0);
        int n = Sqlite.column_bytes(stmt, 0);
        sink.accept(p.reinterpret(n), n);
    }

    private DbException error(String operation, int rc) {
        if (db.equals(NULL)) {
            return new DbException(operation, rc & 0xff, rc, "database is closed");
        }
        return new DbException(operation, rc & 0xff, Sqlite.extended_errcode(db), Sqlite.errmsg(db));
    }

    private static MemorySegment finalizeStmt(MemorySegment stmt) {
        if (!stmt.equals(NULL)) {
            Sqlite.finalize_(stmt);
        }
        return NULL;
    }

    /** Prepares a statement that lives as long as the connection. */
    private MemorySegment prepare(String sql) {
        return prepare(sql, SQLITE_PREPARE_PERSISTENT);
    }

    private MemorySegment prepare(String sql, int flags) {
        try (Arena tmp = Arena.ofConfined()) {
            int rc = Sqlite.prepare_v3(db, tmp.allocateFrom(sql), -1, flags, ppOut, NULL);
            if (rc != SQLITE_OK) {
                throw error("prepare " + sql, rc);
            }
            return ppOut.get(ADDRESS, 0);
        }
    }

    /** Runs a one-off statement to completion and returns column 0 of its first row as text (null if no row). */
    private String queryText(String sql) {
        MemorySegment stmt = prepare(sql, 0);
        try {
            int rc = Sqlite.step(stmt);
            String first = null;
            if (rc == SQLITE_ROW) {
                first = columnText(stmt, 0);
            }
            while (rc == SQLITE_ROW) {
                rc = Sqlite.step(stmt);
            }
            if (rc != SQLITE_DONE) {
                throw error(sql, rc);
            }
            return first;
        } finally {
            Sqlite.finalize_(stmt);
        }
    }

    private static String columnText(MemorySegment stmt, int col) {
        MemorySegment p = Sqlite.column_blob(stmt, col);
        int n = Sqlite.column_bytes(stmt, col);
        if (p.equals(NULL)) {
            return n == 0 ? null : "";
        }
        return new String(p.reinterpret(n).toArray(JAVA_BYTE), StandardCharsets.UTF_8);
    }

    /** {@code EXPLAIN QUERY PLAN} of {@code sql} as an indented tree, one node per line. */
    private String explain(String sql) {
        MemorySegment stmt = prepare("EXPLAIN QUERY PLAN " + sql, 0);
        try {
            Map<Long, Integer> depth = new HashMap<>();
            StringBuilder out = new StringBuilder();
            int rc;
            while ((rc = Sqlite.step(stmt)) == SQLITE_ROW) {
                long id = Sqlite.column_int64(stmt, 0);
                long parent = Sqlite.column_int64(stmt, 1);
                int d = depth.getOrDefault(parent, -1) + 1;
                depth.put(id, d);
                if (!out.isEmpty()) {
                    out.append('\n');
                }
                out.append("  ".repeat(d)).append(columnText(stmt, 3));
            }
            if (rc != SQLITE_DONE) {
                throw error("explain " + sql, rc);
            }
            return out.toString();
        } finally {
            Sqlite.finalize_(stmt);
        }
    }

    /**
     * Fills the OS page cache with the database file, then touches every table and index through mmap and runs
     * each read statement once. Reads only.
     */
    private void prewarm(Path path) {
        long t0 = System.nanoTime();
        long bytes = 0;
        try (FileChannel ch = FileChannel.open(path, StandardOpenOption.READ); Arena tmp = Arena.ofConfined()) {
            ByteBuffer buf = tmp.allocate(PREWARM_READ_BYTES, 4096).asByteBuffer();
            int n;
            while ((n = ch.read(buf)) >= 0) {
                bytes += n;
                buf.clear();
            }
        } catch (IOException e) {
            throw new UncheckedIOException("prewarm read " + path, e);
        }
        long t1 = System.nanoTime();
        for (String sql : Sql.PREWARM) {
            queryText(sql);
        }
        long t2 = System.nanoTime();
        feed(DISCARD_PARTS);
        post(1, DISCARD);
        MemorySegment s = postExistsStmt;
        try {
            Sqlite.bind_int64(s, 1, 1);
            Sqlite.step(s);
        } finally {
            Sqlite.reset(s);
        }
        long t3 = System.nanoTime();
        log.info("prewarm: read {} MiB in {} ms, index scans {} ms, statements {} ms", bytes >> 20,
                (t1 - t0) / 1_000_000, (t2 - t1) / 1_000_000, (t3 - t2) / 1_000_000);
    }
}
