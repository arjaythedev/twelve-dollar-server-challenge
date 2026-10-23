package challenge;

import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CharsetDecoder;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;

final class Json {
    static final Object NULL = new Object();
    private static final int MAX_DEPTH = 256;

    static final class Malformed extends Exception {
        static final Malformed INSTANCE = new Malformed();
        private Malformed() { super(null, null, false, false); }
    }

    private static final CharsetDecoder UTF8 = StandardCharsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT);

    private final String s;
    private int i;

    private Json(String s) { this.s = s; }

    static String utf8(byte[] b, int off, int len) throws Malformed {
        try {
            CharBuffer cb = UTF8.reset().decode(ByteBuffer.wrap(b, off, len));
            return cb.toString();
        } catch (CharacterCodingException e) {
            throw Malformed.INSTANCE;
        }
    }

    static Object parse(String s) throws Malformed {
        Json p = new Json(s);
        p.ws();
        Object v = p.value(0);
        p.ws();
        if (p.i != s.length()) throw Malformed.INSTANCE;
        return v;
    }

    static Object get(Object v, String key) {
        return v instanceof Map<?, ?> m ? m.get(key) : null;
    }

    private void ws() {
        while (i < s.length()) {
            char c = s.charAt(i);
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return;
            i++;
        }
    }

    private Object value(int depth) throws Malformed {
        if (i >= s.length() || depth > MAX_DEPTH) throw Malformed.INSTANCE;
        char c = s.charAt(i);
        switch (c) {
            case '{': return object(depth);
            case '[': return array(depth);
            case '"': return string();
            case 't': literal("true"); return Boolean.TRUE;
            case 'f': literal("false"); return Boolean.FALSE;
            case 'n': literal("null"); return NULL;
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return number();
                throw Malformed.INSTANCE;
        }
    }

    private void literal(String word) throws Malformed {
        if (!s.startsWith(word, i)) throw Malformed.INSTANCE;
        i += word.length();
    }

    private Map<String, Object> object(int depth) throws Malformed {
        i++;
        Map<String, Object> m = new HashMap<>();
        ws();
        if (i < s.length() && s.charAt(i) == '}') { i++; return m; }
        while (true) {
            ws();
            if (i >= s.length() || s.charAt(i) != '"') throw Malformed.INSTANCE;
            String key = string();
            ws();
            if (i >= s.length() || s.charAt(i) != ':') throw Malformed.INSTANCE;
            i++;
            ws();
            m.put(key, value(depth + 1));
            ws();
            if (i >= s.length()) throw Malformed.INSTANCE;
            char c = s.charAt(i++);
            if (c == '}') return m;
            if (c != ',') throw Malformed.INSTANCE;
        }
    }

    private ArrayList<Object> array(int depth) throws Malformed {
        i++;
        ArrayList<Object> a = new ArrayList<>();
        ws();
        if (i < s.length() && s.charAt(i) == ']') { i++; return a; }
        while (true) {
            ws();
            a.add(value(depth + 1));
            ws();
            if (i >= s.length()) throw Malformed.INSTANCE;
            char c = s.charAt(i++);
            if (c == ']') return a;
            if (c != ',') throw Malformed.INSTANCE;
        }
    }

    private String string() throws Malformed {
        i++;
        int start = i;
        while (i < s.length()) {
            char c = s.charAt(i);
            if (c == '"') return s.substring(start, i++);
            if (c == '\\') break;
            if (c < 0x20) throw Malformed.INSTANCE;
            i++;
        }
        StringBuilder sb = new StringBuilder(s.length() - start);
        sb.append(s, start, i);
        while (i < s.length()) {
            char c = s.charAt(i++);
            if (c == '"') return sb.toString();
            if (c < 0x20) throw Malformed.INSTANCE;
            if (c != '\\') { sb.append(c); continue; }
            if (i >= s.length()) throw Malformed.INSTANCE;
            char e = s.charAt(i++);
            switch (e) {
                case '"': sb.append('"'); break;
                case '\\': sb.append('\\'); break;
                case '/': sb.append('/'); break;
                case 'b': sb.append('\b'); break;
                case 'f': sb.append('\f'); break;
                case 'n': sb.append('\n'); break;
                case 'r': sb.append('\r'); break;
                case 't': sb.append('\t'); break;
                case 'u': {
                    if (i + 4 > s.length()) throw Malformed.INSTANCE;
                    int v = 0;
                    for (int k = 0; k < 4; k++) {
                        int d = Character.digit(s.charAt(i++), 16);
                        if (d < 0) throw Malformed.INSTANCE;
                        v = (v << 4) | d;
                    }
                    sb.append((char) v);
                    break;
                }
                default: throw Malformed.INSTANCE;
            }
        }
        throw Malformed.INSTANCE;
    }

    private Double number() throws Malformed {
        int start = i;
        if (s.charAt(i) == '-') i++;
        if (i >= s.length()) throw Malformed.INSTANCE;
        if (s.charAt(i) == '0') {
            i++;
        } else if (s.charAt(i) >= '1' && s.charAt(i) <= '9') {
            while (i < s.length() && isDigit(s.charAt(i))) i++;
        } else {
            throw Malformed.INSTANCE;
        }
        if (i < s.length() && s.charAt(i) == '.') {
            i++;
            if (i >= s.length() || !isDigit(s.charAt(i))) throw Malformed.INSTANCE;
            while (i < s.length() && isDigit(s.charAt(i))) i++;
        }
        if (i < s.length() && (s.charAt(i) == 'e' || s.charAt(i) == 'E')) {
            i++;
            if (i < s.length() && (s.charAt(i) == '+' || s.charAt(i) == '-')) i++;
            if (i >= s.length() || !isDigit(s.charAt(i))) throw Malformed.INSTANCE;
            while (i < s.length() && isDigit(s.charAt(i))) i++;
        }
        return Double.valueOf(s.substring(start, i));
    }

    private static boolean isDigit(char c) { return c >= '0' && c <= '9'; }
}
