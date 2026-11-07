package app;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.ByteBuffer;
import java.util.concurrent.ArrayBlockingQueue;

/**
 * Per-request scratch space: one direct buffer that holds the response being rendered plus the
 * few native staging areas a request needs. Pooled, because 15k idle connections must not each
 * own 80 KB -- but the pool is much larger than the {@link Ctx} pool: an Out is held across the
 * socket write (where a slow client can park us), a Ctx never is.
 *
 * Layout of the buffer:
 * <pre>
 *   0      .. 128    header headroom: the status line + Content-Length are written backwards,
 *                    right-aligned against the body, so a response is always a single write()
 *   128    .. 72 KB  response body
 *   72 KB  .. 76 KB  decoded JWT payload              (native: scanned in place)
 *   76 KB  .. 78 KB  decoded username, if it had escapes (native: bound to SQLite by pointer)
 *   78 KB  .. 80 KB  trimmed post body for the INSERT  (native: bound to SQLite by pointer)
 * </pre>
 * The body region fits the worst case feed: 20 posts whose 500 characters all escape to six bytes.
 */
final class Out {
    static final int BODY_START = 128;
    static final int BODY_LIMIT = 73728;
    static final int JWT_OFF = 73728, JWT_CAP = 4096;
    static final int NAME_OFF = 77824, NAME_CAP = 2048;
    static final int INS_OFF = 79872, INS_CAP = 2048;
    static final int CAP = 81920;

    private static final ArrayBlockingQueue<Out> POOL;
    static {
        int n = Integer.getInteger("app.outs", 64);
        POOL = new ArrayBlockingQueue<>(n);
        for (int i = 0; i < n; i++) POOL.add(new Out());
    }

    final ByteBuffer bb = ByteBuffer.allocateDirect(CAP);
    final MemorySegment seg = MemorySegment.ofBuffer(bb);
    final WriteOp op = new WriteOp(this);

    int pos;                       // write cursor inside the body region
    long userId;                   // from the JWT
    int nameOff, nameLen;          // username bytes, inside this buffer
    int insLen;                    // length of the trimmed body staged at INS_OFF

    static Out acquire() {
        try { return POOL.take(); } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException(e);
        }
    }
    static void release(Out o) { POOL.add(o); }

    void reset() { pos = BODY_START; }

    private void room(int n) {
        if (pos + n > BODY_LIMIT) throw new IllegalStateException("response too large");
    }

    void put(byte[] a) {
        room(a.length);
        MemorySegment.copy(a, 0, seg, ValueLayout.JAVA_BYTE, pos, a.length);
        pos += a.length;
    }
    void putByte(byte v) {
        room(1);
        seg.set(ValueLayout.JAVA_BYTE, pos, v);
        pos++;
    }
    /** Copies a text column straight from SQLite's buffer into the response: no Java object. */
    void putColumn(MemorySegment stmt, int col) {
        MemorySegment p = Sqlite.ccolumnText(stmt, col);   // must come before column_bytes
        int n = Sqlite.ccolumnBytes(stmt, col);
        if (n <= 0) return;
        room(n);
        MemorySegment.copy(p.reinterpret(n), 0, seg, pos, n);
        pos += n;
    }
    /** Stages a text column at INS_OFF so the writer thread can bind it after we drop the Ctx. */
    void stageInsertBody(MemorySegment stmt, int col) {
        MemorySegment p = Sqlite.ccolumnText(stmt, col);
        int n = Sqlite.ccolumnBytes(stmt, col);
        if (n < 0 || n > INS_CAP) throw new IllegalStateException("body too large: " + n);
        MemorySegment.copy(p.reinterpret(n), 0, seg, INS_OFF, n);
        insLen = n;
    }
    void putLong(long v) {
        int d = digits(v);
        room(d);
        int p = pos + d;
        long x = v;
        do { seg.set(ValueLayout.JAVA_BYTE, --p, (byte) ('0' + (int) (x % 10))); x /= 10; } while (x > 0);
        pos += d;
    }

    /**
     * Writes the header block backwards, ending where the body starts, and leaves bb positioned on
     * the whole response. One buffer, one write, exact Content-Length, no memmove.
     */
    void finish(int status, boolean keepAlive, boolean http10) {
        byte[] hdr = (!keepAlive ? Responses.HDR_CLOSE
                : http10 ? Responses.HDR_KEEP_10 : Responses.HDR_KEEP)[status];
        int len = pos - BODY_START;
        int p = BODY_START - 4;
        seg.set(ValueLayout.JAVA_BYTE, p, (byte) '\r');
        seg.set(ValueLayout.JAVA_BYTE, p + 1, (byte) '\n');
        seg.set(ValueLayout.JAVA_BYTE, p + 2, (byte) '\r');
        seg.set(ValueLayout.JAVA_BYTE, p + 3, (byte) '\n');
        int d = digits(len);
        p -= d;
        int q = p + d;
        int x = len;
        do { seg.set(ValueLayout.JAVA_BYTE, --q, (byte) ('0' + x % 10)); x /= 10; } while (x > 0);
        p -= hdr.length;
        MemorySegment.copy(hdr, 0, seg, ValueLayout.JAVA_BYTE, p, hdr.length);
        bb.limit(pos).position(p);
    }

    static int digits(long v) {
        int n = 1;
        long t = 10;
        while (v >= t && n < 19) { n++; t *= 10; }
        return n;
    }
}
