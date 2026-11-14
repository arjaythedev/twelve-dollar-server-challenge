package app;

import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;

/**
 * HTTP/1.1 parsing, in place and byte by byte: the request is never turned into Strings, only into
 * offsets into the connection's own direct buffer. Only the four headers the API needs are looked at.
 */
final class Http {
    private Http() {}

    static final int M_OTHER = 0, M_GET = 1, M_POST = 2;

    /** One per connection, reused for every request on it. */
    static final class Req {
        ByteBuffer buf;
        MemorySegment seg;      // the same buffer as native memory, for SQLite binds
        int method;
        int pathOff, pathLen;
        int authOff, authLen;   // Authorization value, authOff < 0 when absent
        int contentLength;
        int bodyOff;
        boolean keepAlive;
        boolean expectContinue;
        boolean http10;
    }

    private static final byte[] H_CONTENT_LENGTH = ascii("content-length");
    private static final byte[] H_AUTHORIZATION = ascii("authorization");
    private static final byte[] H_CONNECTION = ascii("connection");
    private static final byte[] H_EXPECT = ascii("expect");
    private static final byte[] V_CLOSE = ascii("close");
    private static final byte[] V_KEEP_ALIVE = ascii("keep-alive");
    private static final byte[] V_100_CONTINUE = ascii("100-continue");

    private static byte[] ascii(String s) { return s.getBytes(StandardCharsets.US_ASCII); }

    /** Index just past the CRLFCRLF that ends the head, or -1 if it is not in the buffer yet. */
    static int findHeadEnd(ByteBuffer b, int from, int filled) {
        for (int i = from + 3; i < filled; i++) {
            if (b.get(i) == '\n' && b.get(i - 1) == '\r' && b.get(i - 2) == '\n' && b.get(i - 3) == '\r') {
                return i + 1;
            }
        }
        return -1;
    }

    /** Parses the head in [start, headEnd). False means the request is unusable: answer and close. */
    static boolean parse(Req r, int start, int headEnd) {
        ByteBuffer b = r.buf;
        r.method = M_OTHER;
        r.authOff = -1;
        r.authLen = 0;
        r.contentLength = 0;
        r.expectContinue = false;
        if (headEnd - start < 16) return false;

        int p = start;
        if (b.get(p) == 'G' && b.get(p + 1) == 'E' && b.get(p + 2) == 'T' && b.get(p + 3) == ' ') {
            r.method = M_GET;
            p += 4;
        } else if (b.get(p) == 'P' && b.get(p + 1) == 'O' && b.get(p + 2) == 'S'
                && b.get(p + 3) == 'T' && b.get(p + 4) == ' ') {
            r.method = M_POST;
            p += 5;
        } else {
            return false;
        }

        int pathStart = p;
        while (p < headEnd && b.get(p) != ' ' && b.get(p) != '?') p++;
        r.pathOff = pathStart;
        r.pathLen = p - pathStart;
        if (r.pathLen == 0) return false;
        while (p < headEnd && b.get(p) != ' ') p++;          // skip any query string
        p++;
        if (p + 8 > headEnd) return false;
        r.http10 = b.get(p + 7) == '0';
        r.keepAlive = !r.http10;                             // HTTP/1.0 closes unless told otherwise
        p += 8;
        while (p < headEnd && b.get(p) != '\n') p++;
        p++;

        while (p < headEnd) {
            if (b.get(p) == '\r') break;
            int lineEnd = p;
            while (lineEnd < headEnd && b.get(lineEnd) != '\n') lineEnd++;
            int colon = p;
            while (colon < lineEnd && b.get(colon) != ':') colon++;
            if (colon < lineEnd) {
                int v = colon + 1;
                while (v < lineEnd && (b.get(v) == ' ' || b.get(v) == '\t')) v++;
                int ve = lineEnd;
                while (ve > v) {
                    byte c = b.get(ve - 1);
                    if (c == '\r' || c == ' ' || c == '\t') ve--; else break;
                }
                if (!header(r, p, colon - p, v, ve - v)) return false;
            }
            p = lineEnd + 1;
        }
        return true;
    }

    private static boolean header(Req r, int nameOff, int nameLen, int vOff, int vLen) {
        ByteBuffer b = r.buf;
        switch (nameLen) {
            case 14:
                if (eqLower(b, nameOff, nameLen, H_CONTENT_LENGTH)) {
                    r.contentLength = parseInt(b, vOff, vLen);
                    if (r.contentLength < 0) return false;
                }
                break;
            case 13:
                if (eqLower(b, nameOff, nameLen, H_AUTHORIZATION)) {
                    r.authOff = vOff;
                    r.authLen = vLen;
                }
                break;
            case 10:
                if (eqLower(b, nameOff, nameLen, H_CONNECTION)) {
                    if (hasToken(b, vOff, vLen, V_CLOSE)) r.keepAlive = false;
                    else if (hasToken(b, vOff, vLen, V_KEEP_ALIVE)) r.keepAlive = true;
                }
                break;
            case 6:
                if (eqLower(b, nameOff, nameLen, H_EXPECT) && hasToken(b, vOff, vLen, V_100_CONTINUE)) {
                    r.expectContinue = true;
                }
                break;
            default:
                break;
        }
        return true;
    }

    static boolean eqLower(ByteBuffer b, int off, int len, byte[] lower) {
        if (len != lower.length) return false;
        for (int i = 0; i < len; i++) {
            int c = b.get(off + i) & 0xFF;
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != lower[i]) return false;
        }
        return true;
    }

    /** Case-insensitive search for one token in a comma-separated header value. */
    private static boolean hasToken(ByteBuffer b, int off, int len, byte[] token) {
        int i = off, end = off + len;
        while (i < end) {
            while (i < end && (b.get(i) == ' ' || b.get(i) == '\t' || b.get(i) == ',')) i++;
            int s = i;
            while (i < end && b.get(i) != ',') i++;
            int e = i;
            while (e > s && (b.get(e - 1) == ' ' || b.get(e - 1) == '\t')) e--;
            if (eqLower(b, s, e - s, token)) return true;
        }
        return false;
    }

    private static int parseInt(ByteBuffer b, int off, int len) {
        if (len <= 0 || len > 9) return -1;
        int v = 0;
        for (int i = 0; i < len; i++) {
            int c = b.get(off + i) & 0xFF;
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v;
    }
}
