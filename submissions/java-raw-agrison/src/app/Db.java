package app;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.security.InvalidKeyException;
import java.security.NoSuchAlgorithmException;
import java.util.concurrent.ArrayBlockingQueue;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/**
 * Opens the database: one connection per read context plus one for the writer thread, each with
 * its own persistent statements. All JSON that comes from a row is built by SQLite itself
 * (json_object), so the server never formats a value it read.
 */
final class Db {
    private Db() {}

    private static final String POST_JSON =
        "json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username,"
        + "'like_count',(SELECT count(*) FROM likes l WHERE l.post_id=p.id))";

    // CROSS JOIN pins the join order (posts outer) so the plan never depends on statistics:
    // SCAN p USING INDEX posts_created_at_id_idx, no temp B-tree for the ORDER BY.
    private static final String SQL_FEED =
        "SELECT " + POST_JSON + " FROM posts p CROSS JOIN users u ON u.id=p.user_id"
        + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
    private static final String SQL_POST =
        "SELECT " + POST_JSON + " FROM posts p CROSS JOIN users u ON u.id=p.user_id WHERE p.id=?1";
    private static final String SQL_EXISTS = "SELECT 1 FROM posts WHERE id=?1";
    private static final String SQL_HEALTH = "SELECT 1";

    // The request body is validated and trimmed by SQLite, with exactly Node's semantics:
    // json_valid -> malformed, json_type != 'text' -> missing/not a string, then trim and length.
    private static final String SQL_VALIDATE =
        "WITH b(v) AS (SELECT CASE WHEN json_valid(?1) AND json_type(?1,'$.body')='text'"
        + " THEN trim(json_extract(?1,'$.body'),?2) END)"
        + " SELECT CASE WHEN NOT json_valid(?1) THEN 1"
        + " WHEN v IS NULL OR v='' THEN 2"
        + " WHEN length(v)>500 THEN 3 ELSE 0 END, v FROM b";

    private static final String SQL_INSERT_POST =
        "INSERT INTO posts (user_id,body) VALUES (?1,?2)"
        + " RETURNING json_object('id',id,'body',body,'created_at',created_at,"
        + "'author',?3,'like_count',0)";
    private static final String SQL_INSERT_LIKE =
        "INSERT INTO likes (user_id,post_id) VALUES (?1,?2) ON CONFLICT (user_id,post_id) DO NOTHING";

    /** The characters String.prototype.trim strips (WhiteSpace + LineTerminator), as Node does. */
    private static final String TRIM_CHARS =
        "\t\n\u000B\f\r             "
        + "     　﻿";

    static MemorySegment writeDb;
    static MemorySegment stInsertPost, stInsertLike, stBegin, stCommit, stRollback;
    static MemorySegment trimSet;
    static int trimSetLen;

    private static ArrayBlockingQueue<Ctx> readers;

    static void open(String path) {
        if (Sqlite.initialize() != Sqlite.OK) throw new IllegalStateException("sqlite3_initialize failed");

        MemorySegment ts = Arena.global().allocateFrom(TRIM_CHARS);
        trimSet = ts;
        trimSetLen = (int) ts.byteSize() - 1;   // drop the trailing NUL

        writeDb = openConn(path);
        exec(writeDb, "PRAGMA journal_mode=WAL");
        exec(writeDb, "PRAGMA synchronous=NORMAL");         // rule 6: the minimum that stays durable
        exec(writeDb, "PRAGMA mmap_size=1073741824");
        exec(writeDb, "PRAGMA temp_store=MEMORY");
        exec(writeDb, "PRAGMA cache_size=-16000");
        exec(writeDb, "PRAGMA busy_timeout=5000");
        exec(writeDb, "PRAGMA wal_autocheckpoint=4000");
        exec(writeDb, "PRAGMA journal_size_limit=67108864");
        String mode = queryText(writeDb, "PRAGMA journal_mode");
        if (!"wal".equalsIgnoreCase(mode)) throw new IllegalStateException("journal_mode=" + mode);

        stInsertPost = prepare(writeDb, SQL_INSERT_POST);
        stInsertLike = prepare(writeDb, SQL_INSERT_LIKE);
        stBegin = prepare(writeDb, "BEGIN IMMEDIATE");
        stCommit = prepare(writeDb, "COMMIT");
        stRollback = prepare(writeDb, "ROLLBACK");

        int n = Integer.getInteger("app.readers", 4);
        readers = new ArrayBlockingQueue<>(n);
        for (int i = 0; i < n; i++) readers.add(openReader(path));

        System.err.println("sqlite " + Sqlite.libVersion() + " journal_mode=" + mode
                + " readers=" + n + " outs=" + Integer.getInteger("app.outs", 64));
    }

    private static Ctx openReader(String path) {
        MemorySegment db = openConn(path);
        exec(db, "PRAGMA synchronous=NORMAL");
        exec(db, "PRAGMA mmap_size=1073741824");
        exec(db, "PRAGMA temp_store=MEMORY");
        exec(db, "PRAGMA cache_size=-8000");
        exec(db, "PRAGMA busy_timeout=5000");
        exec(db, "PRAGMA query_only=ON");
        Mac mac;
        try {
            mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(Main.jwtSecret, "HmacSHA256"));
        } catch (NoSuchAlgorithmException | InvalidKeyException e) {
            throw new IllegalStateException(e);
        }
        return new Ctx(db, prepare(db, SQL_FEED), prepare(db, SQL_POST), prepare(db, SQL_EXISTS),
                prepare(db, SQL_VALIDATE), prepare(db, SQL_HEALTH), mac);
    }

    static Ctx acquire() {
        try { return readers.take(); } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException(e);
        }
    }
    static void release(Ctx c) { readers.add(c); }

    private static MemorySegment openConn(String path) {
        try (Arena a = Arena.ofConfined()) {
            MemorySegment out = a.allocate(ValueLayout.ADDRESS);
            MemorySegment cPath = a.allocateFrom(path);
            int rc = Sqlite.openV2(cPath, out, Sqlite.OPEN_READWRITE | Sqlite.OPEN_NOMUTEX, MemorySegment.NULL);
            MemorySegment db = out.get(ValueLayout.ADDRESS, 0);
            if (rc != Sqlite.OK) {
                throw new IllegalStateException("sqlite3_open_v2(" + path + ") rc=" + rc
                        + (db.equals(MemorySegment.NULL) ? "" : ": " + Sqlite.errmsg(db)));
            }
            return db;
        }
    }

    static void exec(MemorySegment db, String sql) {
        try (Arena a = Arena.ofConfined()) {
            int rc = Sqlite.exec(db, a.allocateFrom(sql));
            if (rc != Sqlite.OK) throw new IllegalStateException(sql + " -> rc=" + rc + ": " + Sqlite.errmsg(db));
        }
    }

    static MemorySegment prepare(MemorySegment db, String sql) {
        try (Arena a = Arena.ofConfined()) {
            MemorySegment cSql = a.allocateFrom(sql);
            MemorySegment out = a.allocate(ValueLayout.ADDRESS);
            int rc = Sqlite.prepareV3(db, cSql, (int) cSql.byteSize() - 1, Sqlite.PREPARE_PERSISTENT, out);
            if (rc != Sqlite.OK) {
                throw new IllegalStateException("prepare rc=" + rc + ": " + Sqlite.errmsg(db) + " -- " + sql);
            }
            return out.get(ValueLayout.ADDRESS, 0);
        }
    }

    /** Startup only: reads a single text value (PRAGMA echo). */
    private static String queryText(MemorySegment db, String sql) {
        MemorySegment st = prepare(db, sql);
        try {
            if (Sqlite.step(st) != Sqlite.ROW) return null;
            MemorySegment p = Sqlite.columnText(st, 0);
            int n = Sqlite.columnBytes(st, 0);
            byte[] bytes = p.reinterpret(n).toArray(ValueLayout.JAVA_BYTE);
            return new String(bytes, java.nio.charset.StandardCharsets.UTF_8);
        } finally {
            Sqlite.reset(st);
        }
    }
}
