package feed.http;

import io.netty.buffer.ByteBuf;
import io.netty.buffer.ByteBufAllocator;
import io.netty.buffer.Unpooled;
import io.netty.buffer.UnpooledByteBufAllocator;

import java.nio.charset.StandardCharsets;

/**
 * Response bytes. Only HTTP header text and the fixed error bodies are precomputed (they contain no data); every
 * other body comes from SQLite or from a template plus numbers of the current request.
 *
 * <p>Header block: {@code HTTP/1.1 <code> <reason>\r\nContent-Type: application/json\r\nContent-Length: <n>\r\n}
 * [{@code Connection: close\r\n}] {@code \r\n}.
 */
final class Responses {

    static final int S200 = 0, S201 = 1, S400 = 2, S401 = 3, S404 = 4, S413 = 5, S500 = 6, S503 = 7;
    private static final String[] STATUS_LINES = {
            "200 OK", "201 Created", "400 Bad Request", "401 Unauthorized", "404 Not Found",
            "413 Payload Too Large", "500 Internal Server Error", "503 Service Unavailable"};

    static final int NOT_FOUND = 0, INVALID_POST_ID = 1, POST_NOT_FOUND = 2, MISSING_BEARER = 3, INVALID_TOKEN = 4,
            INVALID_PAYLOAD = 5, MALFORMED_JSON = 6, BODY_REQUIRED = 7, BODY_TOO_LONG = 8, INTERNAL_ERROR = 9,
            PAYLOAD_TOO_LARGE = 10, BAD_REQUEST = 11;
    private static final int[] FIXED_STATUS = {S404, S400, S404, S401, S401, S401, S400, S400, S400, S500, S413, S400};
    private static final String[] FIXED_MESSAGES = {
            "not found", "invalid post id", "post not found", "missing bearer token", "invalid or expired token",
            "invalid token payload", "malformed JSON body", "body is required", "body must be at most 500 characters",
            "internal server error", "payload too large", "bad request"};

    private static final byte[][] PREFIX = new byte[STATUS_LINES.length][];
    private static final byte[] END_KEEP_ALIVE = ascii("\r\n\r\n");
    private static final byte[] END_CLOSE = ascii("\r\nConnection: close\r\n\r\n");
    /** Longest possible header block (any status, 10-digit length, {@code Connection: close}). */
    static final int MAX_HEAD;

    private static final ByteBuf[] FIXED_KEEP_ALIVE = new ByteBuf[FIXED_MESSAGES.length];
    private static final ByteBuf[] FIXED_CLOSE = new ByteBuf[FIXED_MESSAGES.length];
    private static final ByteBuf CONTINUE =
            constant(Unpooled.directBuffer().writeBytes(ascii("HTTP/1.1 100 Continue\r\n\r\n")));

    private static final byte[] LIKED_NEW = ascii("{\"liked\":true,\"already_liked\":false,\"post_id\":");
    private static final byte[] LIKED_REPEAT = ascii("{\"liked\":true,\"already_liked\":true,\"post_id\":");
    private static final byte[] HEALTH_OK = ascii("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    private static final String HEALTH_DEGRADED = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":";

    static {
        int longestPrefix = 0;
        for (int s = 0; s < STATUS_LINES.length; s++) {
            PREFIX[s] = ascii("HTTP/1.1 " + STATUS_LINES[s] + "\r\nContent-Type: application/json\r\nContent-Length: ");
            longestPrefix = Math.max(longestPrefix, PREFIX[s].length);
        }
        MAX_HEAD = longestPrefix + 10 + END_CLOSE.length;
        ByteBufAllocator unpooled = UnpooledByteBufAllocator.DEFAULT;
        for (int i = 0; i < FIXED_MESSAGES.length; i++) {
            byte[] body = ascii("{\"error\":\"" + FIXED_MESSAGES[i] + "\"}");
            FIXED_KEEP_ALIVE[i] = constant(head(unpooled, FIXED_STATUS[i], false, body.length).writeBytes(body));
            FIXED_CLOSE[i] = constant(head(unpooled, FIXED_STATUS[i], true, body.length).writeBytes(body));
        }
    }

    private Responses() {
    }

    /** A fixed error response; the returned buffer is a duplicate of a shared, unreleasable one. */
    static ByteBuf fixed(int which, boolean close) {
        return (close ? FIXED_CLOSE : FIXED_KEEP_ALIVE)[which].duplicate();
    }

    static ByteBuf continue100() {
        return CONTINUE.duplicate();
    }

    /** True for an interim {@code 1xx} response (it precedes the real response and must never be replaced). */
    static boolean isInterim(ByteBuf b) {
        return b.getByte(b.readerIndex() + 9) == '1';
    }

    /** Fixed message index for a {@link feed.auth.JwtVerifier} failure code. */
    static int authError(int jwtResult) {
        return switch (jwtResult) {
            case feed.auth.JwtVerifier.MISSING_BEARER -> MISSING_BEARER;
            case feed.auth.JwtVerifier.INVALID_PAYLOAD -> INVALID_PAYLOAD;
            default -> INVALID_TOKEN;
        };
    }

    /** Fixed message index for a {@link feed.json.BodyParser} failure code. */
    static int bodyError(int parseResult) {
        return switch (parseResult) {
            case feed.json.BodyParser.BODY_REQUIRED -> BODY_REQUIRED;
            case feed.json.BodyParser.TOO_LONG -> BODY_TOO_LONG;
            default -> MALFORMED_JSON;
        };
    }

    /**
     * A pooled direct buffer holding the complete header block for a body of {@code bodyLen} bytes, with exactly
     * {@code bodyLen} writable bytes left for the body.
     */
    static ByteBuf head(ByteBufAllocator alloc, int status, boolean close, int bodyLen) {
        byte[] prefix = PREFIX[status];
        byte[] end = close ? END_CLOSE : END_KEEP_ALIVE;
        int digits = digits(bodyLen);
        ByteBuf b = alloc.directBuffer(prefix.length + digits + end.length + bodyLen);
        b.writeBytes(prefix);
        writeDecimal(b, bodyLen, digits);
        b.writeBytes(end);
        return b;
    }

    /**
     * Writes the header block for a body of {@code bodyLen} bytes so that it ends exactly at {@code bodyStart}
     * (which must be at least {@link #MAX_HEAD}) and returns the index where it starts. Writer index unchanged.
     */
    static int headBefore(ByteBuf b, int bodyStart, int status, boolean close, int bodyLen) {
        byte[] prefix = PREFIX[status];
        byte[] end = close ? END_CLOSE : END_KEEP_ALIVE;
        int digits = digits(bodyLen);
        int start = bodyStart - prefix.length - digits - end.length;
        b.setBytes(start, prefix);
        long v = bodyLen;
        for (int i = start + prefix.length + digits - 1; i >= start + prefix.length; i--) {
            b.setByte(i, (int) ('0' + v % 10));
            v /= 10;
        }
        b.setBytes(start + prefix.length + digits, end);
        return start;
    }

    /** {@code {"liked":true,"already_liked":<!created>,"post_id":<id>}} with status 201 (created) or 200. */
    static ByteBuf like(ByteBufAllocator alloc, boolean created, boolean close, long postId) {
        byte[] template = created ? LIKED_NEW : LIKED_REPEAT;
        int digits = digits(postId);
        ByteBuf b = head(alloc, created ? S201 : S200, close, template.length + digits + 1);
        b.writeBytes(template);
        writeDecimal(b, postId, digits);
        b.writeByte('}');
        return b;
    }

    static ByteBuf healthOk(ByteBufAllocator alloc, boolean close, long uptimeSeconds) {
        int digits = digits(uptimeSeconds);
        ByteBuf b = head(alloc, S200, close, HEALTH_OK.length + digits + 1);
        b.writeBytes(HEALTH_OK);
        writeDecimal(b, uptimeSeconds, digits);
        b.writeByte('}');
        return b;
    }

    static ByteBuf healthDegraded(ByteBufAllocator alloc, boolean close, String error) {
        byte[] body = (HEALTH_DEGRADED + jsonString(error) + "}").getBytes(StandardCharsets.UTF_8);
        ByteBuf b = head(alloc, S503, close, body.length);
        b.writeBytes(body);
        return b;
    }

    /** {@code s} as a JSON string literal (quotes included). */
    static String jsonString(String s) {
        StringBuilder sb = new StringBuilder(s.length() + 2).append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"' -> sb.append("\\\"");
                case '\\' -> sb.append("\\\\");
                case '\n' -> sb.append("\\n");
                case '\r' -> sb.append("\\r");
                case '\t' -> sb.append("\\t");
                default -> {
                    if (c < 0x20 || Character.isSurrogate(c) && !validSurrogateAt(s, i)) {
                        sb.append(String.format("\\u%04x", (int) c));
                    } else {
                        sb.append(c);
                    }
                }
            }
        }
        return sb.append('"').toString();
    }

    private static boolean validSurrogateAt(String s, int i) {
        char c = s.charAt(i);
        if (Character.isHighSurrogate(c)) {
            return i + 1 < s.length() && Character.isLowSurrogate(s.charAt(i + 1));
        }
        return i > 0 && Character.isHighSurrogate(s.charAt(i - 1));
    }

    static int digits(long v) {
        int n = 1;
        while (v >= 10) {
            v /= 10;
            n++;
        }
        return n;
    }

    /** Writes non-negative {@code v}, which has {@code digits} decimal digits, without allocating. */
    static void writeDecimal(ByteBuf b, long v, int digits) {
        int w = b.writerIndex();
        b.ensureWritable(digits);
        for (int i = w + digits - 1; i >= w; i--) {
            b.setByte(i, (int) ('0' + v % 10));
            v /= 10;
        }
        b.writerIndex(w + digits);
    }

    @SuppressWarnings("deprecation")
    private static ByteBuf constant(ByteBuf direct) {
        return Unpooled.unreleasableBuffer(direct.asReadOnly());
    }

    private static byte[] ascii(String s) {
        return s.getBytes(StandardCharsets.US_ASCII);
    }
}
