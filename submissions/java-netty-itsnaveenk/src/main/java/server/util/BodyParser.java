package server.util;

public final class BodyParser {
    private BodyParser() {}

    public sealed interface ParseResult {
        record Success(String body) implements ParseResult {}
        record MalformedJson() implements ParseResult {}
        record BodyRequired() implements ParseResult {}
        record BodyTooLong() implements ParseResult {}
    }

    public static final ParseResult.MalformedJson MALFORMED = new ParseResult.MalformedJson();
    public static final ParseResult.BodyRequired BODY_REQUIRED = new ParseResult.BodyRequired();
    public static final ParseResult.BodyTooLong BODY_TOO_LONG = new ParseResult.BodyTooLong();

    public static ParseResult parsePostBody(String json) {
        if (json == null) return MALFORMED;
        String text = json.trim();
        if (!text.startsWith("{") || !text.endsWith("}")) {
            return MALFORMED;
        }

        int len = text.length();
        int i = 1; // after opening '{'
        String bodyVal = null;
        boolean bodyFieldFound = false;
        boolean bodyIsString = false;

        while (i < len - 1) {
            // Skip whitespace
            while (i < len - 1 && Character.isWhitespace(text.charAt(i))) i++;
            if (i >= len - 1) break;

            if (text.charAt(i) == ',') {
                i++;
                continue;
            }

            // Key must be a quoted string
            if (text.charAt(i) != '"') {
                return MALFORMED;
            }
            int keyStart = ++i;
            while (i < len - 1 && text.charAt(i) != '"') {
                if (text.charAt(i) == '\\') i++; // skip escaped
                i++;
            }
            if (i >= len - 1 || text.charAt(i) != '"') {
                return MALFORMED;
            }
            int keyLen = i - keyStart;
            boolean isBodyKey = (keyLen == 4
                    && text.charAt(keyStart) == 'b'
                    && text.charAt(keyStart + 1) == 'o'
                    && text.charAt(keyStart + 2) == 'd'
                    && text.charAt(keyStart + 3) == 'y');
            i++; // skip closing quote

            // Expect ':'
            while (i < len - 1 && Character.isWhitespace(text.charAt(i))) i++;
            if (i >= len - 1 || text.charAt(i) != ':') {
                return MALFORMED;
            }
            i++; // skip ':'

            // Value
            while (i < len - 1 && Character.isWhitespace(text.charAt(i))) i++;
            if (i >= len - 1) {
                return MALFORMED;
            }

            char c = text.charAt(i);
            if (isBodyKey) {
                bodyFieldFound = true;
            }

            if (c == '"') {
                // String value
                if (isBodyKey) bodyIsString = true;
                int valStart = ++i; // skip opening quote
                boolean hasEscape = false;
                boolean closed = false;

                while (i < len - 1) {
                    char sc = text.charAt(i);
                    if (sc == '\\') {
                        hasEscape = true;
                        i += 2; // skip escape character and escaped char
                        continue;
                    }
                    if (sc == '"') {
                        closed = true;
                        break;
                    }
                    i++;
                }

                if (!closed) {
                    return MALFORMED;
                }

                int valEnd = i++; // point past closing quote

                if (isBodyKey) {
                    if (!hasEscape) {
                        bodyVal = text.substring(valStart, valEnd);
                    } else {
                        // Decode escaped string
                        bodyVal = decodeEscapes(text, valStart, valEnd);
                        if (bodyVal == null) {
                            return MALFORMED;
                        }
                    }
                }
            } else if (c == '{' || c == '[') {
                // Object or Array - skip nested structure
                char open = c;
                char close = (c == '{') ? '}' : ']';
                int depth = 1;
                i++;
                while (i < len - 1 && depth > 0) {
                    char sc = text.charAt(i);
                    if (sc == open) depth++;
                    else if (sc == close) depth--;
                    else if (sc == '"') {
                        i++;
                        while (i < len - 1 && text.charAt(i) != '"') {
                            if (text.charAt(i) == '\\') i++;
                            i++;
                        }
                    }
                    i++;
                }
                if (depth != 0) {
                    return MALFORMED;
                }
            } else {
                // Number, true, false, null
                int valStart = i;
                while (i < len - 1 && text.charAt(i) != ',' && text.charAt(i) != '}' && !Character.isWhitespace(text.charAt(i))) {
                    i++;
                }
                String literal = text.substring(valStart, i);
                if (!isValidLiteralOrNumber(literal)) {
                    return MALFORMED;
                }
            }
        }

        if (!bodyFieldFound) {
            return BODY_REQUIRED;
        }

        if (!bodyIsString) {
            return BODY_REQUIRED;
        }

        // Apply JS trim
        String trimmed = StringUtil.trimJs(bodyVal);
        if (trimmed == null || trimmed.isEmpty()) {
            return BODY_REQUIRED;
        }

        // Check length limit of 500 characters
        if (trimmed.length() > 500) {
            return BODY_TOO_LONG;
        }

        return new ParseResult.Success(trimmed);
    }

    private static String decodeEscapes(String text, int start, int end) {
        StringBuilder sb = new StringBuilder(end - start);
        for (int i = start; i < end; i++) {
            char c = text.charAt(i);
            if (c == '\\') {
                if (i + 1 >= end) return null;
                char next = text.charAt(++i);
                switch (next) {
                    case '"' -> sb.append('"');
                    case '\\' -> sb.append('\\');
                    case '/' -> sb.append('/');
                    case 'b' -> sb.append('\b');
                    case 'f' -> sb.append('\f');
                    case 'n' -> sb.append('\n');
                    case 'r' -> sb.append('\r');
                    case 't' -> sb.append('\t');
                    case 'u' -> {
                        if (i + 4 < end) {
                            try {
                                sb.append((char) Integer.parseInt(text.substring(i + 1, i + 5), 16));
                                i += 4;
                            } catch (NumberFormatException e) {
                                return null;
                            }
                        } else {
                            return null;
                        }
                    }
                    default -> sb.append(next);
                }
            } else {
                sb.append(c);
            }
        }
        return sb.toString();
    }

    private static boolean isValidLiteralOrNumber(String literal) {
        if ("true".equals(literal) || "false".equals(literal) || "null".equals(literal)) {
            return true;
        }
        try {
            Double.parseDouble(literal);
            return true;
        } catch (NumberFormatException e) {
            return false;
        }
    }
}
