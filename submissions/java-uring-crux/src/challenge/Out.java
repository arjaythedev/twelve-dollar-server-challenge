package challenge;

import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;

/** One response. java.nio copies it off the heap. io_uring builds it into a caller-owned segment. */
final class Out {
    static final byte[] S200 = status("200 OK");
    static final byte[] S201 = status("201 Created");
    static final byte[] S400 = status("400 Bad Request");
    static final byte[] S401 = status("401 Unauthorized");
    static final byte[] S404 = status("404 Not Found");
    static final byte[] S500 = status("500 Internal Server Error");
    static final byte[] S503 = status("503 Service Unavailable");
    private static final byte[] CLOSE = ascii("Connection: close\r\n");
    private static final byte[] LENGTH = ascii("Content-Length: ");
    private static final byte[] END = ascii("\r\n\r\n");

    private byte[] buf = new byte[256];
    private final byte[] digits = new byte[20];
    /** When set, puts go here and the segment stays stable for an in-flight send. */
    private MemorySegment mem;
    private int pos;
    boolean close;

    static byte[] ascii(String s) { return s.getBytes(StandardCharsets.US_ASCII); }

    private static byte[] status(String s) {
        return ascii("HTTP/1.1 " + s + "\r\nContent-Type: application/json\r\n");
    }

    void reset() { pos = 0; close = false; }

    /** Heap mode used by java.nio and by any response that is copied out as a byte array. */
    void useHeap() {
        mem = null;
        if (buf == null) buf = new byte[256];
        pos = 0;
        close = false;
    }

    /** Off-heap mode. The caller keeps {@code dest} alive until the send completes. */
    void use(MemorySegment dest) {
        mem = dest;
        pos = 0;
        close = false;
    }

    int length() { return pos; }

    boolean direct() { return mem != null; }

    int room() { return mem == null ? 0 : (int) mem.byteSize() - pos; }

    /** Bytes still free in the native buffer. Valid only when {@link #direct()} is true. */
    MemorySegment remaining() { return mem.asSlice(pos, mem.byteSize() - pos); }

    void advance(int n) { pos += n; }

    /** Copies the finished heap response into {@code dest}. Returns -1 if it does not fit. */
    int copyInto(MemorySegment dest) {
        if (pos > dest.byteSize()) return -1;
        if (pos > 0) MemorySegment.copy(MemorySegment.ofArray(buf), JAVA_BYTE, 0, dest, JAVA_BYTE, 0, pos);
        return pos;
    }

    int mark() { return pos; }

    void rewind(int mark) { pos = mark; }

    byte[] toByteArray() {
        byte[] out = new byte[pos];
        System.arraycopy(buf, 0, out, 0, pos);
        return out;
    }

    /**
     * One allocation: status line in front of the body already in this buffer.
     * Content-Length is the body size.
     */
    byte[] framed(byte[] status) {
        int bodyLen = pos;
        int headerLen = status.length + (close ? CLOSE.length : 0) + LENGTH.length + digits(bodyLen) + END.length;
        byte[] exact = new byte[headerLen + bodyLen];
        int p = copy(exact, 0, status);
        if (close) p = copy(exact, p, CLOSE);
        p = copy(exact, p, LENGTH);
        int n = digits(bodyLen);
        int q = p + n;
        long v = bodyLen;
        do {
            exact[--q] = (byte) ('0' + (v % 10));
            v /= 10;
        } while (v != 0);
        p += n;
        p = copy(exact, p, END);
        System.arraycopy(buf, 0, exact, p, bodyLen);
        pos = 0;
        return exact;
    }

    /** Slides the body forward and writes the status line in front of it, in this same buffer. */
    void frameInPlace(byte[] status) {
        int bodyLen = pos;
        int headerLen = status.length + (close ? CLOSE.length : 0) + LENGTH.length + digits(bodyLen) + END.length;
        need(headerLen);
        shift(headerLen, bodyLen);
        pos = 0;
        head(status, bodyLen);
        pos = headerLen + bodyLen;
    }

    private void shift(int dest, int len) {
        if (len == 0 || dest == 0) return;
        if (mem != null) {
            MemorySegment.copy(mem, 0, mem, dest, len);
            return;
        }
        System.arraycopy(buf, 0, buf, dest, len);
    }

    private static int copy(byte[] dst, int pos, byte[] src) {
        System.arraycopy(src, 0, dst, pos, src.length);
        return pos + src.length;
    }

    void respond(byte[] status, byte[] body) {
        head(status, body.length);
        put(body);
    }

    void head(byte[] status, int bodyLength) {
        put(status);
        if (close) put(CLOSE);
        put(LENGTH);
        putLong(bodyLength);
        put(END);
    }

    void put(byte[] a) { put(a, 0, a.length); }

    void put(byte[] a, int off, int len) {
        if (len <= 0) return;
        need(len);
        if (mem != null) MemorySegment.copy(MemorySegment.ofArray(a), JAVA_BYTE, off, mem, JAVA_BYTE, pos, len);
        else System.arraycopy(a, off, buf, pos, len);
        pos += len;
    }

    void putSegment(MemorySegment src, int len) {
        if (len <= 0) return;
        need(len);
        if (mem != null) MemorySegment.copy(src, 0, mem, pos, len);
        else MemorySegment.copy(src, JAVA_BYTE, 0, buf, pos, len);
        pos += len;
    }

    void putByte(int b) {
        need(1);
        if (mem != null) mem.set(JAVA_BYTE, pos, (byte) b);
        else buf[pos] = (byte) b;
        pos++;
    }

    void putLong(long v) {
        int p = digits.length;
        do {
            digits[--p] = (byte) ('0' + (v % 10));
            v /= 10;
        } while (v != 0);
        put(digits, p, digits.length - p);
    }

    static int digits(long v) {
        int n = 1;
        while (v >= 10) { v /= 10; n++; }
        return n;
    }

    private void need(int n) {
        int cap = mem != null ? (int) mem.byteSize() : buf.length;
        if (pos + n <= cap) return;
        if (mem != null) throw new IndexOutOfBoundsException("response buffer");
        while (cap < pos + n) cap *= 2;
        byte[] next = new byte[cap];
        System.arraycopy(buf, 0, next, 0, pos);
        buf = next;
    }
}
