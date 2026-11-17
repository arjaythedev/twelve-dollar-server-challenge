package challenge;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * A small strict JSON parser (objects become maps, numbers become doubles, null becomes {@link #NULL}).
 * The inputs here are tiny (a request body, a JWT header or payload), so a hand-rolled parser beats
 * spinning up a general-purpose data binder. Responses are written by {@link Out}.
 */
final class Json {

    static final Object NULL = new Object();
    private static final int MAX_DEPTH = 64;

    static final class Malformed extends Exception {
        Malformed() {
            super(null, null, false, false);
        }
    }

    private static final Malformed MALFORMED = new Malformed();

    private final String text;
    private int pos;

    private Json(String text) {
        this.text = text;
    }

    static Object parse(String s) throws Malformed {
        Json p = new Json(s);
        Object v = p.value(0);
        p.ws();
        if (p.pos != s.length()) throw MALFORMED;
        return v;
    }

    private void ws() {
        while (pos < text.length()) {
            char c = text.charAt(pos);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') pos++;
            else break;
        }
    }

    private Object value(int depth) throws Malformed {
        if (depth > MAX_DEPTH) throw MALFORMED;
        ws();
        if (pos >= text.length()) throw MALFORMED;
        char c = text.charAt(pos);
        switch (c) {
            case '{': return object(depth);
            case '[': return array(depth);
            case '"': return string();
            case 't': return literal("true", Boolean.TRUE);
            case 'f': return literal("false", Boolean.FALSE);
            case 'n': return literal("null", NULL);
            default: return number();
        }
    }

    private Object literal(String word, Object v) throws Malformed {
        if (!text.startsWith(word, pos)) throw MALFORMED;
        pos += word.length();
        return v;
    }

    private Map<String, Object> object(int depth) throws Malformed {
        Map<String, Object> m = new HashMap<>();
        pos++; // {
        ws();
        if (pos < text.length() && text.charAt(pos) == '}') {
            pos++;
            return m;
        }
        while (true) {
            ws();
            if (pos >= text.length() || text.charAt(pos) != '"') throw MALFORMED;
            String key = string();
            ws();
            if (pos >= text.length() || text.charAt(pos) != ':') throw MALFORMED;
            pos++;
            m.put(key, value(depth + 1));
            ws();
            if (pos >= text.length()) throw MALFORMED;
            char c = text.charAt(pos++);
            if (c == '}') return m;
            if (c != ',') throw MALFORMED;
        }
    }

    private List<Object> array(int depth) throws Malformed {
        List<Object> a = new ArrayList<>();
        pos++; // [
        ws();
        if (pos < text.length() && text.charAt(pos) == ']') {
            pos++;
            return a;
        }
        while (true) {
            a.add(value(depth + 1));
            ws();
            if (pos >= text.length()) throw MALFORMED;
            char c = text.charAt(pos++);
            if (c == ']') return a;
            if (c != ',') throw MALFORMED;
        }
    }

    private String string() throws Malformed {
        pos++; // opening quote
        int start = pos;
        // fast path: no escapes
        while (pos < text.length()) {
            char c = text.charAt(pos);
            if (c == '"') return text.substring(start, pos++);
            if (c == '\\' || c < 0x20) break;
            pos++;
        }
        StringBuilder sb = new StringBuilder(text.length() - start);
        sb.append(text, start, pos);
        while (pos < text.length()) {
            char c = text.charAt(pos++);
            if (c == '"') return sb.toString();
            if (c < 0x20) throw MALFORMED;
            if (c != '\\') {
                sb.append(c);
                continue;
            }
            if (pos >= text.length()) throw MALFORMED;
            char e = text.charAt(pos++);
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
                    if (pos + 4 > text.length()) throw MALFORMED;
                    int cp = 0;
                    for (int k = 0; k < 4; k++) {
                        int d = Character.digit(text.charAt(pos++), 16);
                        if (d < 0) throw MALFORMED;
                        cp = (cp << 4) | d;
                    }
                    sb.append((char) cp);
                    break;
                }
                default: throw MALFORMED;
            }
        }
        throw MALFORMED;
    }

    private Double number() throws Malformed {
        int start = pos;
        if (pos < text.length() && text.charAt(pos) == '-') pos++;
        if (pos >= text.length()) throw MALFORMED;
        if (text.charAt(pos) == '0') pos++;
        else if (!digits()) throw MALFORMED;
        if (pos < text.length() && text.charAt(pos) == '.') {
            pos++;
            if (!digits()) throw MALFORMED;
        }
        if (pos < text.length() && (text.charAt(pos) == 'e' || text.charAt(pos) == 'E')) {
            pos++;
            if (pos < text.length() && (text.charAt(pos) == '+' || text.charAt(pos) == '-')) pos++;
            if (!digits()) throw MALFORMED;
        }
        return Double.parseDouble(text.substring(start, pos));
    }

    private boolean digits() {
        int start = pos;
        while (pos < text.length() && text.charAt(pos) >= '0' && text.charAt(pos) <= '9') pos++;
        return pos > start;
    }
}
