package server.json;

import io.netty.buffer.ByteBuf;
import io.netty.buffer.ByteBufUtil;
import server.db.Database;

import java.nio.charset.StandardCharsets;
import java.util.List;

public final class FastJsonWriter {
    private FastJsonWriter() {}

    private static final byte[] HEALTH_PREFIX = "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] HEALTH_DEGRADED_PREFIX = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] ERROR_PREFIX = "{\"error\":".getBytes(StandardCharsets.US_ASCII);

    private static final byte[] POST_OBJ_PREFIX = "{\"id\":".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] BODY_PREFIX = ",\"body\":".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] CREATED_PREFIX = ",\"created_at\":".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] AUTHOR_PREFIX = ",\"author\":".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] LIKE_PREFIX = ",\"like_count\":".getBytes(StandardCharsets.US_ASCII);

    private static final byte[] POST_WRAPPER_PREFIX = "{\"post\":".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] FEED_WRAPPER_PREFIX = "{\"posts\":[".getBytes(StandardCharsets.US_ASCII);

    private static final byte[] LIKE_RESP_PREFIX = "{\"liked\":true,\"already_liked\":".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] POST_ID_PREFIX = ",\"post_id\":".getBytes(StandardCharsets.US_ASCII);

    private static final byte[] TRUE_BYTES = "true".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] FALSE_BYTES = "false".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] NULL_BYTES = "null".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] MIN_VALUE_BYTES = "-9223372036854775808".getBytes(StandardCharsets.US_ASCII);

    private static final byte[] ESCAPED_QUOTE = "\\\"".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] ESCAPED_BACKSLASH = "\\\\".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] ESCAPED_B = "\\b".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] ESCAPED_F = "\\f".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] ESCAPED_N = "\\n".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] ESCAPED_R = "\\r".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] ESCAPED_T = "\\t".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] UNICODE_PREFIX = "\\u00".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] HEX_CHARS = "0123456789abcdef".getBytes(StandardCharsets.US_ASCII);

    private static final ThreadLocal<byte[]> DIGITS_BUF = ThreadLocal.withInitial(() -> new byte[32]);

    public static void writeHealth(ByteBuf buf, int uptimeSeconds) {
        buf.writeBytes(HEALTH_PREFIX);
        writeLong(buf, uptimeSeconds);
        buf.writeByte('}');
    }

    public static void writeHealthDegraded(ByteBuf buf, String error) {
        buf.writeBytes(HEALTH_DEGRADED_PREFIX);
        writeJsonString(buf, error);
        buf.writeByte('}');
    }

    public static void writeError(ByteBuf buf, String message) {
        buf.writeBytes(ERROR_PREFIX);
        writeJsonString(buf, message);
        buf.writeByte('}');
    }

    public static void writePostObject(ByteBuf buf, long id, String body, String createdAt, String author, int likeCount) {
        buf.writeBytes(POST_OBJ_PREFIX);
        writeLong(buf, id);
        buf.writeBytes(BODY_PREFIX);
        writeJsonString(buf, body);
        buf.writeBytes(CREATED_PREFIX);
        writeAsciiQuoted(buf, createdAt);
        buf.writeBytes(AUTHOR_PREFIX);
        writeJsonString(buf, author);
        buf.writeBytes(LIKE_PREFIX);
        writeLong(buf, likeCount);
        buf.writeByte('}');
    }

    public static void writePostResponse(ByteBuf buf, long id, String body, String createdAt, String author, int likeCount) {
        buf.writeBytes(POST_WRAPPER_PREFIX);
        writePostObject(buf, id, body, createdAt, author, likeCount);
        buf.writeByte('}');
    }

    public static void writeFeedResponse(ByteBuf buf, List<Database.Post> posts) {
        buf.writeBytes(FEED_WRAPPER_PREFIX);
        int size = posts.size();
        for (int i = 0; i < size; i++) {
            if (i > 0) {
                buf.writeByte(',');
            }
            Database.Post p = posts.get(i);
            writePostObject(buf, p.id(), p.body(), p.createdAt(), p.author(), p.likeCount());
        }
        buf.writeByte(']');
        buf.writeByte('}');
    }

    public static void writeLikeResponse(ByteBuf buf, boolean alreadyLiked, long postId) {
        buf.writeBytes(LIKE_RESP_PREFIX);
        buf.writeBytes(alreadyLiked ? TRUE_BYTES : FALSE_BYTES);
        buf.writeBytes(POST_ID_PREFIX);
        writeLong(buf, postId);
        buf.writeByte('}');
    }

    public static void writeLong(ByteBuf buf, long v) {
        if (v == Long.MIN_VALUE) {
            buf.writeBytes(MIN_VALUE_BYTES);
            return;
        }
        if (v < 0) {
            buf.writeByte('-');
            v = -v;
        }
        if (v < 10) {
            buf.writeByte((byte) ('0' + v));
            return;
        }
        byte[] digits = DIGITS_BUF.get();
        int charPos = 32;
        while (v >= 10) {
            long q = v / 10;
            digits[--charPos] = (byte) ('0' + (v - q * 10));
            v = q;
        }
        digits[--charPos] = (byte) ('0' + v);
        buf.writeBytes(digits, charPos, 32 - charPos);
    }

    public static void writeAsciiQuoted(ByteBuf buf, String s) {
        if (s == null) {
            buf.writeBytes(NULL_BYTES);
            return;
        }
        buf.writeByte('"');
        ByteBufUtil.writeAscii(buf, s);
        buf.writeByte('"');
    }

    public static void writeJsonString(ByteBuf buf, String s) {
        if (s == null) {
            buf.writeBytes(NULL_BYTES);
            return;
        }
        buf.writeByte('"');
        int len = s.length();
        int mark = 0;
        for (int i = 0; i < len; i++) {
            char c = s.charAt(i);
            if (c == '"' || c == '\\' || c < 0x20) {
                if (i > mark) {
                    ByteBufUtil.writeUtf8(buf, s, mark, i);
                }
                switch (c) {
                    case '"' -> buf.writeBytes(ESCAPED_QUOTE);
                    case '\\' -> buf.writeBytes(ESCAPED_BACKSLASH);
                    case '\b' -> buf.writeBytes(ESCAPED_B);
                    case '\f' -> buf.writeBytes(ESCAPED_F);
                    case '\n' -> buf.writeBytes(ESCAPED_N);
                    case '\r' -> buf.writeBytes(ESCAPED_R);
                    case '\t' -> buf.writeBytes(ESCAPED_T);
                    default -> {
                        buf.writeBytes(UNICODE_PREFIX);
                        buf.writeByte(HEX_CHARS[(c >> 4) & 0xF]);
                        buf.writeByte(HEX_CHARS[c & 0xF]);
                    }
                }
                mark = i + 1;
            }
        }
        if (len > mark) {
            ByteBufUtil.writeUtf8(buf, s, mark, len);
        }
        buf.writeByte('"');
    }
}
