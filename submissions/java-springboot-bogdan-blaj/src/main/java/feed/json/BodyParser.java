package feed.json;

/**
 * Validates a create-post request body and extracts the trimmed {@code body} string as UTF-8.
 * Single-threaded and reusable; allocation-free once its output buffer has grown to the largest body seen.
 */
public final class BodyParser {
    public static final int OK = 0, MALFORMED = 1, BODY_REQUIRED = 2, TOO_LONG = 3;
    public static final int MAX_CODE_POINTS = 500;

    private static final byte[] KEY_BODY = {'b', 'o', 'd', 'y'};

    private final JsonScanner scanner = new JsonScanner(KEY_BODY);
    private byte[] out = new byte[1024];
    private int outLength;

    /** Parses the whole request body {@code buf[off, off+len)}. On OK, {@link #body()} holds the trimmed value. */
    public int parse(byte[] buf, int off, int len) {
        outLength = 0;
        if (!scanner.scan(buf, off, len)) return MALFORMED;
        if (scanner.topType() != JsonScanner.OBJECT || scanner.type(0) != JsonScanner.STRING) return BODY_REQUIRED;
        byte[] src = buf;
        int s = scanner.start(0);
        int e = scanner.end(0);
        if (scanner.escaped(0)) {
            if (out.length < e - s) out = new byte[Math.max(e - s, out.length * 2)];
            e = JsonScanner.decodeString(buf, s, e, out, 0);
            s = 0;
            src = out;
        }
        int ts = JsTrim.trimStart(src, s, e);
        int te = JsTrim.trimEnd(src, ts, e);
        int n = te - ts;
        if (n == 0) return BODY_REQUIRED;
        // Each code point takes at least one byte, so counting is only needed above the limit.
        if (n > MAX_CODE_POINTS && Utf8.codePointCount(src, ts, te) > MAX_CODE_POINTS) return TOO_LONG;
        if (out.length < n) out = new byte[Math.max(n, out.length * 2)];
        System.arraycopy(src, ts, out, 0, n);
        outLength = n;
        return OK;
    }

    /** UTF-8 of the trimmed body after {@link #parse} returned OK; valid until the next parse. */
    public byte[] body() {
        return out;
    }

    public int bodyLength() {
        return outLength;
    }
}
