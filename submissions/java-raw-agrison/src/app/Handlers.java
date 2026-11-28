package app;

import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;

/**
 * The five endpoints. Every byte that comes from the database is produced by SQLite's json_object
 * and copied straight into the response buffer; the server only writes the fixed frames around it.
 *
 * On the authenticated routes the order is auth, then id, then body -- what the spec asks for.
 */
final class Handlers {
    private Handlers() {}

    private static final long MAX_POST_ID = (1L << 53);

    static int feed(Out out) {
        Ctx c = Db.acquire();
        try {
            MemorySegment st = c.stFeed;
            out.put(Responses.FEED_OPEN);
            int rc;
            boolean first = true;
            while ((rc = Sqlite.cstep(st)) == Sqlite.ROW) {
                if (first) first = false; else out.putByte((byte) ',');
                out.putColumn(st, 0);
            }
            Sqlite.creset(st);
            if (rc != Sqlite.DONE) return internal(out, c, "feed");
            out.put(Responses.FEED_CLOSE);
            return Responses.S200;
        } finally {
            Db.release(c);
        }
    }

    static int post(Http.Req r, int segOff, int segLen, Out out) {
        long id = parseId(r.buf, segOff, segLen);
        if (id <= 0) return error(out, Responses.ERR_INVALID_ID, Responses.S400);
        Ctx c = Db.acquire();
        try {
            MemorySegment st = c.stPost;
            Sqlite.cbindLong(st, 1, id);
            int rc = Sqlite.cstep(st);
            if (rc == Sqlite.ROW) {
                out.put(Responses.POST_OPEN);
                out.putColumn(st, 0);
                out.putByte((byte) '}');
                Sqlite.creset(st);
                return Responses.S200;
            }
            Sqlite.creset(st);
            if (rc == Sqlite.DONE) return error(out, Responses.ERR_POST_NOT_FOUND, Responses.S404);
            return internal(out, c, "post");
        } finally {
            Db.release(c);
        }
    }

    static int health(Out out) {
        Ctx c = Db.acquire();
        boolean ok;
        try {
            int rc = Sqlite.cstep(c.stHealth);
            ok = rc == Sqlite.ROW && Sqlite.ccolumnLong(c.stHealth, 0) == 1;
            Sqlite.creset(c.stHealth);
        } catch (Throwable t) {
            ok = false;
        } finally {
            Db.release(c);
        }
        if (!ok) return error(out, Responses.HEALTH_BAD, Responses.S503);
        out.put(Responses.HEALTH_OK);
        out.putLong((System.nanoTime() - Main.START_NANOS) / 1_000_000_000L);
        out.putByte((byte) '}');
        return Responses.S200;
    }

    static int create(Http.Req r, Out out) {
        int code;
        Ctx c = Db.acquire();
        try {
            int auth = Jwt.verify(r, out, c, nowSeconds());
            if (auth != Jwt.OK) return authError(out, auth);
            // SQLite validates and trims the body: json_valid / json_type / trim / length.
            MemorySegment st = c.stValidate;
            Sqlite.cbindText(st, 1, r.seg, r.bodyOff, r.contentLength);
            Sqlite.cbindText(st, 2, Db.trimSet, 0, Db.trimSetLen);
            int rc = Sqlite.cstep(st);
            if (rc != Sqlite.ROW) {
                Sqlite.creset(st);
                return internal(out, c, "validate");
            }
            code = (int) Sqlite.ccolumnLong(st, 0);
            if (code == 0) out.stageInsertBody(st, 1);
            Sqlite.creset(st);          // releases the bind on the request buffer
        } finally {
            Db.release(c);
        }
        switch (code) {
            case 1: return error(out, Responses.ERR_BAD_JSON, Responses.S400);
            case 2: return error(out, Responses.ERR_BODY_REQUIRED, Responses.S400);
            case 3: return error(out, Responses.ERR_BODY_TOO_LONG, Responses.S400);
            default: break;
        }
        out.put(Responses.POST_OPEN);
        WriteOp op = out.op;
        op.kind = WriteOp.CREATE;
        op.userId = out.userId;
        op.bodyLen = out.insLen;
        op.nameOff = out.nameOff;
        op.nameLen = out.nameLen;
        op.outPos = out.pos;
        if (!Writer.run(op) || op.outLen == 0) return internal(out, null, "insert post");
        out.pos = op.outPos + op.outLen;
        out.putByte((byte) '}');
        return Responses.S201;
    }

    static int like(Http.Req r, int segOff, int segLen, Out out) {
        long id;
        Ctx c = Db.acquire();
        try {
            int auth = Jwt.verify(r, out, c, nowSeconds());
            if (auth != Jwt.OK) return authError(out, auth);
            id = parseId(r.buf, segOff, segLen);
            if (id <= 0) return error(out, Responses.ERR_INVALID_ID, Responses.S400);
            // Posts are never deleted, so checking outside the write transaction is safe.
            Sqlite.cbindLong(c.stExists, 1, id);
            int rc = Sqlite.cstep(c.stExists);
            Sqlite.creset(c.stExists);
            if (rc == Sqlite.DONE) return error(out, Responses.ERR_POST_NOT_FOUND, Responses.S404);
            if (rc != Sqlite.ROW) return internal(out, c, "like exists");
        } finally {
            Db.release(c);
        }
        WriteOp op = out.op;
        op.kind = WriteOp.LIKE;
        op.userId = out.userId;
        op.postId = id;
        if (!Writer.run(op)) return internal(out, null, "insert like");
        out.put(Responses.LIKE_OPEN);
        out.put(op.alreadyLiked ? Responses.TRUE : Responses.FALSE);
        out.put(Responses.LIKE_MID);
        out.putLong(id);
        out.putByte((byte) '}');
        return op.alreadyLiked ? Responses.S200 : Responses.S201;
    }

    static int notFound(Out out) { return error(out, Responses.ERR_NOT_FOUND, Responses.S404); }

    static int error(Out out, byte[] body, int status) {
        out.reset();
        out.put(body);
        return status;
    }

    private static int authError(Out out, int code) {
        byte[] body = code == Jwt.MISSING ? Responses.ERR_NO_BEARER
                : code == Jwt.BAD_PAYLOAD ? Responses.ERR_BAD_PAYLOAD
                : Responses.ERR_BAD_TOKEN;
        return error(out, body, Responses.S401);
    }

    private static int internal(Out out, Ctx c, String what) {
        System.err.println("500 " + what + (c == null ? "" : ": " + Sqlite.errmsg(c.db)));
        return error(out, Responses.ERR_INTERNAL, Responses.S500);
    }

    /** Positive decimal only: "0", "-1", "abc" and "1.5" are all invalid (400). */
    private static long parseId(ByteBuffer b, int off, int len) {
        if (len <= 0 || len > 16) return -1;
        long v = 0;
        for (int i = 0; i < len; i++) {
            int ch = b.get(off + i) & 0xFF;
            if (ch < '0' || ch > '9') return -1;
            v = v * 10 + (ch - '0');
        }
        return v > 0 && v <= MAX_POST_ID ? v : -1;
    }

    private static long nowSeconds() { return System.currentTimeMillis() / 1000L; }
}
