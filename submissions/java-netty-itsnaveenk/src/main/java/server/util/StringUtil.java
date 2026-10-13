package server.util;

public final class StringUtil {
    private StringUtil() {}

    /**
     * Matches JavaScript's String.prototype.trim() whitespace specification
     * (WhiteSpace + LineTerminator).
     */
    public static boolean isJsWhitespace(char c) {
        if (c <= ' ') {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\u000B' || c == '\u000C';
        }
        return switch (c) {
            case '\u00A0', '\u1680', '\u2000', '\u2001', '\u2002', '\u2003', '\u2004',
                 '\u2005', '\u2006', '\u2007', '\u2008', '\u2009', '\u200A',
                 '\u2028', '\u2029', '\u202F', '\u205F', '\u3000', '\uFEFF' -> true;
            default -> false;
        };
    }

    public static String trimJs(String s) {
        if (s == null) return null;
        int len = s.length();
        int start = 0;
        while (start < len && isJsWhitespace(s.charAt(start))) {
            start++;
        }
        if (start == len) {
            return "";
        }
        int end = len - 1;
        while (end > start && isJsWhitespace(s.charAt(end))) {
            end--;
        }
        return (start == 0 && end == len - 1) ? s : s.substring(start, end + 1);
    }

    /**
     * Parses a positive integer string in plain decimal digits.
     * Returns null if not a positive integer (e.g. "0", "-1", "abc", "1.5", empty).
     */
    public static Long parsePositiveId(String raw) {
        if (raw == null || raw.isEmpty()) return null;
        int len = raw.length();
        if (len > 19) return null; // Avoid overflow
        long val = 0;
        for (int i = 0; i < len; i++) {
            char c = raw.charAt(i);
            if (c < '0' || c > '9') return null;
            val = val * 10 + (c - '0');
            if (val < 0) return null; // Overflow
        }
        return val > 0 ? val : null;
    }
}
