package server.util;

public final class BodyParser {
    private BodyParser() {}

    public sealed interface ParseResult {
        record Success(String body) implements ParseResult {}
        record MalformedJson() implements ParseResult {}
        record BodyRequired() implements ParseResult {}
        record BodyTooLong() implements ParseResult {}
    }

    public static ParseResult parsePostBody(String json) {
        if (json == null) return new ParseResult.MalformedJson();
        String text = json.trim();
        if (!text.startsWith("{") || !text.endsWith("}")) {
            return new ParseResult.MalformedJson();
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
                return new ParseResult.MalformedJson();
            }
            int keyStart = ++i;
            while (i < len - 1 && text.charAt(i) != '"') {
                if (text.charAt(i) == '\\') i++; // skip escaped
                i++;
            }
            if (i >= len - 1 || text.charAt(i) != '"') {
                return new ParseResult.MalformedJson();
            }
            String key = text.substring(keyStart, i);
            i++; // skip closing quote

            // Expect ':'
            while (i < len - 1 && Character.isWhitespace(text.charAt(i))) i++;
            if (i >= len - 1 || text.charAt(i) != ':') {
                return new ParseResult.MalformedJson();
            }
            i++; // skip ':'

            // Value
            while (i < len - 1 && Character.isWhitespace(text.charAt(i))) i++;
            if (i >= len - 1) {
                return new ParseResult.MalformedJson();
            }

            char c = text.charAt(i);
            boolean isBodyKey = "body".equals(key);
            if (isBodyKey) {
                bodyFieldFound = true;
            }

            if (c == '"') {
                // String value
                if (isBodyKey) bodyIsString = true;
                i++; // skip opening quote
                StringBuilder sb = new StringBuilder();
                boolean escaped = false;
                boolean closed = false;
                while (i < len - 1) {
                    char sc = text.charAt(i);
                    if (escaped) {
                        switch (sc) {
                            case '"' -> sb.append('"');
                            case '\\' -> sb.append('\\');
                            case '/' -> sb.append('/');
                            case 'b' -> sb.append('\b');
                            case 'f' -> sb.append('\f');
                            case 'n' -> sb.append('\n');
                            case 'r' -> sb.append('\r');
                            case 't' -> sb.append('\t');
                            case 'u' -> {
                                if (i + 4 < len - 1) {
                                    try {
                                        sb.append((char) Integer.parseInt(text.substring(i + 1, i + 5), 16));
                                        i += 4;
                                    } catch (NumberFormatException e) {
                                        return new ParseResult.MalformedJson();
                                    }
                                } else {
                                    return new ParseResult.MalformedJson();
                                }
                            }
                            default -> sb.append(sc);
                        }
                        escaped = false;
                    } else if (sc == '\\') {
                        escaped = true;
                    } else if (sc == '"') {
                        closed = true;
                        i++;
                        break;
                    } else {
                        sb.append(sc);
                    }
                    i++;
                }
                if (!closed) {
                    return new ParseResult.MalformedJson();
                }
                if (isBodyKey) {
                    bodyVal = sb.toString();
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
                    return new ParseResult.MalformedJson();
                }
            } else if (Character.isDigit(c) || c == '-' || c == 't' || c == 'f' || c == 'n') {
                // Number or boolean/null
                int valStart = i;
                while (i < len - 1 && text.charAt(i) != ',' && text.charAt(i) != '}' && !Character.isWhitespace(text.charAt(i))) {
                    i++;
                }
                String val = text.substring(valStart, i);
                if (!"true".equals(val) && !"false".equals(val) && !"null".equals(val)) {
                    // verify number
                    try {
                        Double.parseDouble(val);
                    } catch (NumberFormatException e) {
                        return new ParseResult.MalformedJson();
                    }
                }
            } else {
                return new ParseResult.MalformedJson();
            }

            // Skip whitespace up to next ',' or '}'
            while (i < len - 1 && Character.isWhitespace(text.charAt(i))) i++;
            if (i < len - 1 && text.charAt(i) == ',') {
                i++;
            } else if (i < len - 1 && text.charAt(i) != '}') {
                return new ParseResult.MalformedJson();
            }
        }

        if (!bodyFieldFound || !bodyIsString) {
            return new ParseResult.BodyRequired();
        }

        String trimmed = StringUtil.trimJs(bodyVal);
        if (trimmed == null || trimmed.isEmpty()) {
            return new ParseResult.BodyRequired();
        }

        if (trimmed.length() > 500) {
            return new ParseResult.BodyTooLong();
        }

        return new ParseResult.Success(trimmed);
    }
}
