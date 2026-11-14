package challenge;

import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;

final class Out {
    static final byte[] S200 = status("200 OK"), S201 = status("201 Created"), S400 = status("400 Bad Request"),
            S401 = status("401 Unauthorized"), S404 = status("404 Not Found"), S413 = status("413 Content Too Large"),
            S431 = status("431 Request Header Fields Too Large"), S500 = status("500 Internal Server Error"),
            S501 = status("501 Not Implemented"), S503 = status("503 Service Unavailable");
    private static final byte[] CLOSE = ascii("Connection: close\r\n");
    private static final byte[] LENGTH = ascii("Content-Length: ");
    private static final byte[] END = ascii("\r\n\r\n");

    final ByteBuffer buf;
    final MemorySegment seg;
    private final long base;
    int pos;
    boolean close;

    Out(int capacity) {
        buf = ByteBuffer.allocateDirect(capacity);
        seg = MemorySegment.ofBuffer(buf);
        base = seg.address();
    }

    int capacity() { return buf.capacity(); }

    long address(int at) { return base + at; }

    static byte[] ascii(String s) { return s.getBytes(StandardCharsets.US_ASCII); }

    private static byte[] status(String s) {
        return ascii("HTTP/1.1 " + s + "\r\nContent-Type: application/json\r\n");
    }

    int free() { return buf.capacity() - pos; }

    void head(byte[] status, int bodyLength) {
        put(status);
        if (close) put(CLOSE);
        put(LENGTH);
        putLong(bodyLength);
        put(END);
    }

    /**
     * Writes a head whose body is about to be written by native code, with room for a length of the given
     * number of digits, and returns where the body goes. {@link #endBody} fills the length in.
     */
    int headForBody(byte[] status, int digits) {
        put(status);
        if (close) put(CLOSE);
        put(LENGTH);
        pos += digits;
        put(END);
        return pos;
    }

    /**
     * Fills in the length of a body of n bytes at bodyAt and returns its digit count, the best guess for the
     * next one. A wrong guess moves the body. A feed's length rarely changes digit count, so this is rare.
     */
    int endBody(int bodyAt, int n, int guessed) {
        int digits = digits(n);
        int lengthAt = bodyAt - END.length - guessed;
        if (digits != guessed) {
            MemorySegment.copy(seg, bodyAt, seg, lengthAt + digits + END.length, n);
            buf.put(lengthAt + digits, END);
        }
        pos = lengthAt;
        putLong(n);
        pos = lengthAt + digits + END.length + n;
        return digits;
    }

    void respond(byte[] status, byte[] body) {
        head(status, body.length);
        put(body);
    }

    void put(byte[] a) {
        buf.put(pos, a);
        pos += a.length;
    }

    void put(byte[] a, int off, int len) {
        buf.put(pos, a, off, len);
        pos += len;
    }

    void putLong(long v) {
        int n = digits(v);
        int p = pos + n;
        do {
            buf.put(--p, (byte) ('0' + (v % 10)));
            v /= 10;
        } while (v != 0);
        pos += n;
    }

    void copyNative(MemorySegment p, int n) {
        MemorySegment.copy(p.reinterpret(n), 0, seg, pos, n);
        pos += n;
    }

    static int digits(long v) {
        int n = 1;
        while (v >= 10) { v /= 10; n++; }
        return n;
    }
}
