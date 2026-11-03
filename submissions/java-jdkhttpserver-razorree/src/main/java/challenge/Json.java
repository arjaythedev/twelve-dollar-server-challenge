package challenge;

import com.fasterxml.jackson.core.*;
import com.fasterxml.jackson.core.util.JsonRecyclerPools;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.sql.SQLException;
import java.util.*;

final class Json {
    private static final byte[] FEED_PREFIX = "{\"posts\":[".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] POST_PREFIX = "{\"post\":".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] ID = "{\"id\":".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] BODY = ",\"body\":".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] CREATED = ",\"created_at\":".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] AUTHOR = ",\"author\":".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] LIKES = ",\"like_count\":".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] MIN_LONG = "-9223372036854775808".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] HEX = "0123456789ABCDEF".getBytes(StandardCharsets.US_ASCII);
    private static final JsonFactory FACTORY = JsonFactory.builder()
        .recyclerPool(JsonRecyclerPools.newBoundedPool(64))
        .enable(StreamReadFeature.STRICT_DUPLICATE_DETECTION).build();

    static Object parse(byte[] input) throws IOException {
        try (JsonParser p = FACTORY.createParser(input)) {
            JsonToken token = p.nextToken();
            if (token == null) throw new IOException("Empty JSON");
            Object value = read(p, token);
            if (p.nextToken() != null) throw new IOException("Trailing JSON");
            return value;
        }
    }

    private static Object read(JsonParser p, JsonToken token) throws IOException {
        return switch (token) {
            case START_OBJECT -> {
                Map<String, Object> map = new LinkedHashMap<>();
                while (p.nextToken() != JsonToken.END_OBJECT) {
                    if (p.currentToken() != JsonToken.FIELD_NAME) throw new IOException("Expected field");
                    String key = validString(p.currentName());
                    JsonToken next = p.nextToken();
                    if (next == null) throw new IOException("Truncated object");
                    map.put(key, read(p, next));
                }
                yield map;
            }
            case START_ARRAY -> {
                List<Object> values = new ArrayList<>();
                JsonToken next;
                while ((next = p.nextToken()) != JsonToken.END_ARRAY) {
                    if (next == null) throw new IOException("Truncated array");
                    values.add(read(p, next));
                }
                yield values;
            }
            case VALUE_STRING -> validString(p.getText());
            case VALUE_NUMBER_INT, VALUE_NUMBER_FLOAT -> p.getDecimalValue();
            case VALUE_TRUE -> Boolean.TRUE;
            case VALUE_FALSE -> Boolean.FALSE;
            case VALUE_NULL -> null;
            default -> throw new IOException("Invalid JSON token");
        };
    }

    private static String validString(String s) throws IOException {
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (Character.isHighSurrogate(c)) {
                if (++i >= s.length() || !Character.isLowSurrogate(s.charAt(i)))
                    throw new IOException("Unpaired surrogate");
            } else if (Character.isLowSurrogate(c)) throw new IOException("Unpaired surrogate");
        }
        return s;
    }

    static Map<String, Object> object(Object... fields) {
        Map<String, Object> map = new LinkedHashMap<>();
        for (int i = 0; i < fields.length; i += 2) map.put((String) fields[i], fields[i + 1]);
        return map;
    }

    static byte[] encode(Object value) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (JsonGenerator g = FACTORY.createGenerator(bytes)) { write(g, value); }
        return bytes.toByteArray();
    }

    static byte[] feedBytes(Sqlite.Rows rows) throws SQLException {
        ByteArrayOutputStream out = new ByteArrayOutputStream(4096);
        byte[] digits = new byte[20];
        out.writeBytes(FEED_PREFIX);
        boolean first = true;
        while (rows.next()) {
            if (!first) out.write(',');
            first = false;
            writePost(out, rows, digits);
        }
        out.write(']'); out.write('}');
        return out.toByteArray();
    }

    static byte[] postBytes(Sqlite.Rows row) throws SQLException {
        ByteArrayOutputStream out = new ByteArrayOutputStream(256);
        out.writeBytes(POST_PREFIX);
        writePost(out, row, new byte[20]);
        out.write('}');
        return out.toByteArray();
    }

    private static void writePost(ByteArrayOutputStream out, Sqlite.Rows row, byte[] digits) throws SQLException {
        out.writeBytes(ID);
        writeLong(out, row.getLong(1), digits);
        out.writeBytes(BODY);
        writeUtf8String(out, row.getBytes(2));
        out.writeBytes(CREATED);
        writeUtf8String(out, row.getBytes(3));
        out.writeBytes(AUTHOR);
        writeUtf8String(out, row.getBytes(4));
        out.writeBytes(LIKES);
        writeLong(out, row.getLong(5), digits);
        out.write('}');
    }

    private static void writeLong(ByteArrayOutputStream out, long value, byte[] digits) {
        if (value == Long.MIN_VALUE) { out.writeBytes(MIN_LONG); return; }
        if (value < 0) { out.write('-'); value = -value; }
        int end = digits.length, position = end;
        do {
            digits[--position] = (byte) ('0' + value % 10);
            value /= 10;
        } while (value != 0);
        out.write(digits, position, end - position);
    }

    private static void writeUtf8String(ByteArrayOutputStream out, byte[] utf8) {
        if (utf8 == null) { out.writeBytes("null".getBytes(StandardCharsets.US_ASCII)); return; }
        out.write('"');
        int start = 0;
        for (int i = 0; i < utf8.length; i++) {
            int c = utf8[i] & 255;
            if (c >= 0xF0 && c <= 0xF4 && i + 3 < utf8.length) {
                if (i > start) out.write(utf8, start, i - start);
                int point = ((c & 7) << 18) | ((utf8[i + 1] & 63) << 12)
                    | ((utf8[i + 2] & 63) << 6) | (utf8[i + 3] & 63);
                int pair = point - 0x10000;
                writeUnicodeEscape(out, 0xD800 | (pair >>> 10));
                writeUnicodeEscape(out, 0xDC00 | (pair & 1023));
                i += 3;
                start = i + 1;
                continue;
            }
            if (c >= 32 && c != '"' && c != '\\') continue;
            if (i > start) out.write(utf8, start, i - start);
            out.write('\\');
            switch (c) {
                case '"', '\\' -> out.write(c);
                case 8 -> out.write('b');
                case 9 -> out.write('t');
                case 10 -> out.write('n');
                case 12 -> out.write('f');
                case 13 -> out.write('r');
                default -> {
                    out.write('u'); out.write('0'); out.write('0');
                    out.write(HEX[c >>> 4]); out.write(HEX[c & 15]);
                }
            }
            start = i + 1;
        }
        if (start < utf8.length) out.write(utf8, start, utf8.length - start);
        out.write('"');
    }

    private static void writeUnicodeEscape(ByteArrayOutputStream out, int value) {
        out.write('\\'); out.write('u');
        out.write(HEX[(value >>> 12) & 15]);
        out.write(HEX[(value >>> 8) & 15]);
        out.write(HEX[(value >>> 4) & 15]);
        out.write(HEX[value & 15]);
    }

    private static void write(JsonGenerator g, Object value) throws IOException {
        switch (value) {
            case null -> g.writeNull();
            case String s -> g.writeString(s);
            case Boolean b -> g.writeBoolean(b);
            case Integer i -> g.writeNumber(i);
            case Long l -> g.writeNumber(l);
            case Database.Post post -> {
                g.writeStartObject();
                g.writeNumberField("id", post.id());
                g.writeStringField("body", post.body());
                g.writeStringField("created_at", post.createdAt());
                g.writeStringField("author", post.author());
                g.writeNumberField("like_count", post.likeCount());
                g.writeEndObject();
            }
            case Map<?, ?> map -> {
                g.writeStartObject();
                for (var e : map.entrySet()) {
                    g.writeFieldName((String) e.getKey());
                    write(g, e.getValue());
                }
                g.writeEndObject();
            }
            case List<?> list -> {
                g.writeStartArray();
                for (Object item : list) write(g, item);
                g.writeEndArray();
            }
            default -> throw new IOException("Unsupported JSON value");
        }
    }
}
