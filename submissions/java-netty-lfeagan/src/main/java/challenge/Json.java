package challenge;

import io.netty.buffer.ByteBuf;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;

/**
 * A strict RFC 8259 parser for the two small JSON documents we read (request bodies and JWT
 * header/payload), and the escaping writer for strings in responses.
 *
 * Parsed values are Map, ArrayList, String, Double, Boolean, or {@link #NULL}.
 */
final class Json {
    static final Object NULL = new Object();

    static final class ParseException extends Exception {
        ParseException() { super(null, null, false, false); }
    }

    private static final ParseException ERROR = new ParseException();
    private static final int MAX_DEPTH = 64;

    private final byte[] b;
    private int pos;
    private final int end;

    private Json(byte[] b, int off, int len) {
        this.b = b;
        this.pos = off;
        this.end = off + len;
    }

    static Object parse(byte[] b, int off, int len) throws ParseException {
        Json p = new Json(b, off, len);
        p.ws();
        Object v = p.value(0);
        p.ws();
        if (p.pos != p.end) throw ERROR;
        return v;
    }

    private void ws() {
        while (pos < end) {
            byte c = b[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') pos++;
            else return;
        }
    }

    private Object value(int depth) throws ParseException {
        if (pos >= end || depth > MAX_DEPTH) throw ERROR;
        switch (b[pos]) {
            case '{': return object(depth);
            case '[': return array(depth);
            case '"': return string();
            case 't': literal("true"); return Boolean.TRUE;
            case 'f': literal("false"); return Boolean.FALSE;
            case 'n': literal("null"); return NULL;
            default: return number();
        }
    }

    private void literal(String word) throws ParseException {
        int n = word.length();
        if (end - pos < n) throw ERROR;
        for (int i = 0; i < n; i++) if (b[pos + i] != word.charAt(i)) throw ERROR;
        pos += n;
    }

    private Map<String, Object> object(int depth) throws ParseException {
        pos++; // {
        Map<String, Object> map = new HashMap<>();
        ws();
        if (pos < end && b[pos] == '}') { pos++; return map; }
        while (true) {
            ws();
            if (pos >= end || b[pos] != '"') throw ERROR;
            String key = string();
            ws();
            if (pos >= end || b[pos] != ':') throw ERROR;
            pos++;
            ws();
            map.put(key, value(depth + 1));
            ws();
            if (pos >= end) throw ERROR;
            byte c = b[pos++];
            if (c == '}') return map;
            if (c != ',') throw ERROR;
        }
    }

    private ArrayList<Object> array(int depth) throws ParseException {
        pos++; // [
        ArrayList<Object> list = new ArrayList<>();
        ws();
        if (pos < end && b[pos] == ']') { pos++; return list; }
        while (true) {
            ws();
            list.add(value(depth + 1));
            ws();
            if (pos >= end) throw ERROR;
            byte c = b[pos++];
            if (c == ']') return list;
            if (c != ',') throw ERROR;
        }
    }

    private String string() throws ParseException {
        int start = ++pos; // opening quote
        // fast path: no escapes
        while (pos < end) {
            byte c = b[pos];
            if (c == '"') { pos++; return new String(b, start, pos - 1 - start, StandardCharsets.UTF_8); }
            if (c == '\\') break;
            if ((c & 0xff) < 0x20) throw ERROR;
            pos++;
        }
        if (pos >= end) throw ERROR;
        StringBuilder sb = new StringBuilder(new String(b, start, pos - start, StandardCharsets.UTF_8));
        int runStart = pos;
        while (pos < end) {
            byte c = b[pos];
            if (c == '"') {
                sb.append(new String(b, runStart, pos - runStart, StandardCharsets.UTF_8));
                pos++;
                return sb.toString();
            }
            if ((c & 0xff) < 0x20) throw ERROR;
            if (c != '\\') { pos++; continue; }
            sb.append(new String(b, runStart, pos - runStart, StandardCharsets.UTF_8));
            if (++pos >= end) throw ERROR;
            switch (b[pos++]) {
                case '"': sb.append('"'); break;
                case '\\': sb.append('\\'); break;
                case '/': sb.append('/'); break;
                case 'b': sb.append('\b'); break;
                case 'f': sb.append('\f'); break;
                case 'n': sb.append('\n'); break;
                case 'r': sb.append('\r'); break;
                case 't': sb.append('\t'); break;
                case 'u': {
                    if (end - pos < 4) throw ERROR;
                    int cp = 0;
                    for (int i = 0; i < 4; i++) {
                        int d = Character.digit(b[pos++], 16);
                        if (d < 0) throw ERROR;
                        cp = cp << 4 | d;
                    }
                    sb.append((char) cp);
                    break;
                }
                default: throw ERROR;
            }
            runStart = pos;
        }
        throw ERROR;
    }

    private Double number() throws ParseException {
        int start = pos;
        if (pos < end && b[pos] == '-') pos++;
        if (pos >= end) throw ERROR;
        if (b[pos] == '0') pos++;
        else if (b[pos] >= '1' && b[pos] <= '9') digits();
        else throw ERROR;
        if (pos < end && b[pos] == '.') { pos++; requireDigits(); }
        if (pos < end && (b[pos] == 'e' || b[pos] == 'E')) {
            pos++;
            if (pos < end && (b[pos] == '+' || b[pos] == '-')) pos++;
            requireDigits();
        }
        return Double.valueOf(new String(b, start, pos - start, StandardCharsets.ISO_8859_1));
    }

    private void requireDigits() throws ParseException {
        if (pos >= end || b[pos] < '0' || b[pos] > '9') throw ERROR;
        digits();
    }

    private void digits() {
        while (pos < end && b[pos] >= '0' && b[pos] <= '9') pos++;
    }

    // ---- writing ----

    private static final byte[] HEX = "0123456789abcdef".getBytes(StandardCharsets.US_ASCII);

    /** Writes UTF-8 bytes as the inside of a JSON string, escaping quotes, backslashes and control characters. */
    static void writeEscaped(ByteBuf out, byte[] b, int off, int len) {
        int runStart = off, end = off + len;
        for (int i = off; i < end; i++) {
            int c = b[i] & 0xff;
            if (c >= 0x20 && c != '"' && c != '\\') continue;
            if (i > runStart) out.writeBytes(b, runStart, i - runStart);
            out.writeByte('\\');
            switch (c) {
                case '"': out.writeByte('"'); break;
                case '\\': out.writeByte('\\'); break;
                case '\n': out.writeByte('n'); break;
                case '\r': out.writeByte('r'); break;
                case '\t': out.writeByte('t'); break;
                case '\b': out.writeByte('b'); break;
                case '\f': out.writeByte('f'); break;
                default:
                    out.writeByte('u').writeByte('0').writeByte('0').writeByte(HEX[c >> 4]).writeByte(HEX[c & 0xf]);
            }
            runStart = i + 1;
        }
        if (end > runStart) out.writeBytes(b, runStart, end - runStart);
    }

    static void writeEscaped(ByteBuf out, String s) {
        byte[] b = s.getBytes(StandardCharsets.UTF_8);
        writeEscaped(out, b, 0, b.length);
    }

    static void writeLong(ByteBuf out, long v) {
        if (v < 0) {
            out.writeByte('-');
            if (v == Long.MIN_VALUE) { out.writeCharSequence("9223372036854775808", StandardCharsets.US_ASCII); return; }
            v = -v;
        }
        if (v < 10) { out.writeByte((int) ('0' + v)); return; }
        int digits = 0;
        for (long t = v; t > 0; t /= 10) digits++;
        int at = out.writerIndex();
        out.ensureWritable(digits);
        for (int i = digits - 1; i >= 0; i--) {
            out.setByte(at + i, (int) ('0' + v % 10));
            v /= 10;
        }
        out.writerIndex(at + digits);
    }
}
