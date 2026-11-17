package challenge;

import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/**
 * A reusable byte buffer that response JSON is written into. Text from SQLite arrives as raw UTF-8
 * bytes ({@code ResultSet.getBytes}) and is escaped byte by byte, so it is never decoded into a String
 * and re-encoded. JSON escaping is UTF-8 safe at the byte level: only {@code "}, {@code \} and bytes
 * below 0x20 are escaped, and those never occur inside a multi-byte sequence.
 */
final class Out {

    private static final byte[] HEX = "0123456789abcdef".getBytes(StandardCharsets.US_ASCII);

    private byte[] buf = new byte[8192];
    private int len;

    Out reset() {
        len = 0;
        return this;
    }

    private void ensure(int extra) {
        if (len + extra > buf.length) buf = Arrays.copyOf(buf, Math.max(buf.length * 2, len + extra));
    }

    /** Appends constant ASCII text verbatim. */
    Out ascii(String s) {
        int count = s.length();
        ensure(count);
        for (int i = 0; i < count; i++) buf[len + i] = (byte) s.charAt(i); // allocation-free; ASCII only
        len += count;
        return this;
    }

    Out ch(char c) {
        ensure(1);
        buf[len++] = (byte) c;
        return this;
    }

    Out num(long v) {
        ensure(20);
        if (v == 0) {
            buf[len++] = '0';
            return this;
        }
        if (v < 0) {
            buf[len++] = '-';
            v = -v;
        }
        int start = len;
        while (v > 0) {
            buf[len++] = (byte) ('0' + (v % 10));
            v /= 10;
        }
        for (int i = start, j = len - 1; i < j; i++, j--) {
            byte t = buf[i];
            buf[i] = buf[j];
            buf[j] = t;
        }
        return this;
    }

    /** Appends UTF-8 bytes as a quoted JSON string. */
    Out str(byte[] s) {
        ensure(s.length + 2);
        buf[len++] = '"';
        int from = 0;
        for (int i = 0; i < s.length; i++) {
            int c = s[i];
            if (c == '"' || c == '\\' || (c >= 0 && c < 0x20)) {
                int run = i - from;
                ensure(run + 6);
                System.arraycopy(s, from, buf, len, run);
                len += run;
                escape(c);
                from = i + 1;
            }
        }
        int run = s.length - from;
        ensure(run + 1);
        System.arraycopy(s, from, buf, len, run);
        len += run;
        buf[len++] = '"';
        return this;
    }

    /**
     * Appends {@code size} UTF-8 bytes from native memory as a quoted JSON string: one bulk copy into
     * the buffer, then a scan; text that needs escaping (rare) takes the slow path.
     */
    Out str(MemorySegment src, int size) {
        ensure(size + 2);
        int start = len + 1;
        Sqlite.copy(src, buf, start, size);
        for (int i = start, end = start + size; i < end; i++) {
            int c = buf[i];
            if (c == '"' || c == '\\' || (c >= 0 && c < 0x20)) {
                return str(Arrays.copyOfRange(buf, start, end)); // overwrites the copy at len
            }
        }
        buf[len] = '"';
        len = start + size;
        buf[len++] = '"';
        return this;
    }

    Out str(String s) {
        return str(s.getBytes(StandardCharsets.UTF_8));
    }

    private void escape(int c) {
        buf[len++] = '\\';
        switch (c) {
            case '"' -> buf[len++] = '"';
            case '\\' -> buf[len++] = '\\';
            case '\n' -> buf[len++] = 'n';
            case '\r' -> buf[len++] = 'r';
            case '\t' -> buf[len++] = 't';
            case '\b' -> buf[len++] = 'b';
            case '\f' -> buf[len++] = 'f';
            default -> {
                buf[len++] = 'u';
                buf[len++] = '0';
                buf[len++] = '0';
                buf[len++] = HEX[c >> 4];
                buf[len++] = HEX[c & 0xF];
            }
        }
    }

    /** Appends a post object, keys in the order the spec requires. */
    Out post(long id, byte[] body, byte[] createdAt, byte[] author, long likeCount) {
        ascii("{\"id\":").num(id).ascii(",\"body\":").str(body)
                .ascii(",\"created_at\":").str(createdAt)
                .ascii(",\"author\":").str(author)
                .ascii(",\"like_count\":").num(likeCount);
        return ch('}');
    }

    Resp resp(int status) {
        return new Resp(status, Arrays.copyOf(buf, len));
    }
}
