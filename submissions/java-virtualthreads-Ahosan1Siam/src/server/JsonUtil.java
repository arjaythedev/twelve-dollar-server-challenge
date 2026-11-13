package server;

public final class JsonUtil {
    private JsonUtil() {}

    public static class PostBodyResult {
        public String body;
        public int status = 200;
        public String errorJson;
    }

    public static PostBodyResult parseCreatePostBody(String json) {
        PostBodyResult res = new PostBodyResult();
        if (json == null) {
            res.status = 400;
            res.errorJson = "{\"error\":\"malformed JSON body\"}";
            return res;
        }

        String trimmed = json.trim();
        if (!trimmed.startsWith("{") || !trimmed.endsWith("}")) {
            res.status = 400;
            res.errorJson = "{\"error\":\"malformed JSON body\"}";
            return res;
        }

        int keyIdx = trimmed.indexOf("\"body\"");
        if (keyIdx == -1) {
            res.status = 400;
            res.errorJson = "{\"error\":\"body is required\"}";
            return res;
        }

        int colon = trimmed.indexOf(':', keyIdx + 6);
        if (colon == -1) {
            res.status = 400;
            res.errorJson = "{\"error\":\"malformed JSON body\"}";
            return res;
        }

        int valStart = colon + 1;
        while (valStart < trimmed.length() && Character.isWhitespace(trimmed.charAt(valStart))) {
            valStart++;
        }

        if (valStart >= trimmed.length()) {
            res.status = 400;
            res.errorJson = "{\"error\":\"malformed JSON body\"}";
            return res;
        }

        if (trimmed.charAt(valStart) != '"') {
            res.status = 400;
            res.errorJson = "{\"error\":\"body is required\"}";
            return res;
        }

        StringBuilder sb = new StringBuilder();
        int i = valStart + 1;
        boolean closed = false;
        while (i < trimmed.length()) {
            char c = trimmed.charAt(i);
            if (c == '"') {
                closed = true;
                i++;
                break;
            } else if (c == '\\') {
                i++;
                if (i >= trimmed.length()) {
                    res.status = 400;
                    res.errorJson = "{\"error\":\"malformed JSON body\"}";
                    return res;
                }
                char esc = trimmed.charAt(i);
                switch (esc) {
                    case '"' -> sb.append('"');
                    case '\\' -> sb.append('\\');
                    case '/' -> sb.append('/');
                    case 'b' -> sb.append('\b');
                    case 'f' -> sb.append('\f');
                    case 'n' -> sb.append('\n');
                    case 'r' -> sb.append('\r');
                    case 't' -> sb.append('\t');
                    case 'u' -> {
                        if (i + 4 >= trimmed.length()) {
                            res.status = 400;
                            res.errorJson = "{\"error\":\"malformed JSON body\"}";
                            return res;
                        }
                        String hex = trimmed.substring(i + 1, i + 5);
                        try {
                            sb.append((char) Integer.parseInt(hex, 16));
                            i += 4;
                        } catch (NumberFormatException e) {
                            res.status = 400;
                            res.errorJson = "{\"error\":\"malformed JSON body\"}";
                            return res;
                        }
                    }
                    default -> sb.append(esc);
                }
                i++;
            } else {
                sb.append(c);
                i++;
            }
        }

        if (!closed) {
            res.status = 400;
            res.errorJson = "{\"error\":\"malformed JSON body\"}";
            return res;
        }

        while (i < trimmed.length()) {
            char c = trimmed.charAt(i);
            if (c == '}' || Character.isWhitespace(c) || c == ',') {
                i++;
            } else {
                res.status = 400;
                res.errorJson = "{\"error\":\"malformed JSON body\"}";
                return res;
            }
        }

        String body = sb.toString().trim();
        if (body.isEmpty()) {
            res.status = 400;
            res.errorJson = "{\"error\":\"body is required\"}";
            return res;
        }

        if (body.length() > 500) {
            res.status = 400;
            res.errorJson = "{\"error\":\"body must be at most 500 characters\"}";
            return res;
        }

        res.body = body;
        return res;
    }

    public static String escapeJson(String s) {
        if (s == null) return "null";
        StringBuilder sb = new StringBuilder(s.length() + 16);
        sb.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"' -> sb.append("\\\"");
                case '\\' -> sb.append("\\\\");
                case '\b' -> sb.append("\\b");
                case '\f' -> sb.append("\\f");
                case '\n' -> sb.append("\\n");
                case '\r' -> sb.append("\\r");
                case '\t' -> sb.append("\\t");
                default -> {
                    if (c < 32) {
                        sb.append(String.format("\\u%04x", (int) c));
                    } else {
                        sb.append(c);
                    }
                }
            }
        }
        sb.append('"');
        return sb.toString();
    }
}

