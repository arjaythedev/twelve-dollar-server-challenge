package challenge;

import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;

/**
 * Turns one complete HTTP/1.1 request into a response. Parsing does not touch SQLite.
 * {@link #execute} runs inside the caller's transaction.
 */
final class App {
    static final int INCOMPLETE = 0;

    private static final int MAX_HEAD = 16 * 1024;
    private static final int MAX_BODY = 32 * 1024;

    private static final int NOT_FOUND = 0, HEALTH = 1, FEED = 2, GET_POST = 3, CREATE = 4, LIKE = 5;

    private static final byte[] E_NOT_FOUND = err("not found");
    private static final byte[] E_INVALID_ID = err("invalid post id");
    private static final byte[] E_NO_POST = err("post not found");
    private static final byte[] E_MISSING_TOKEN = err("missing bearer token");
    private static final byte[] E_INVALID_TOKEN = err("invalid or expired token");
    private static final byte[] E_BAD_PAYLOAD = err("invalid token payload");
    private static final byte[] E_MALFORMED = err("malformed JSON body");
    private static final byte[] E_BODY_REQUIRED = err("body is required");
    private static final byte[] E_BODY_TOO_LONG = err("body must be at most 500 characters");
    private static final byte[] E_INTERNAL = err("internal server error");
    private static final byte[] LIKED_NEW = Out.ascii("{\"liked\":true,\"already_liked\":false,\"post_id\":");
    private static final byte[] LIKED_AGAIN = Out.ascii("{\"liked\":true,\"already_liked\":true,\"post_id\":");
    private static final byte[] HEALTH_OK = Out.ascii("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    private static final byte[] CLOSE_BRACE = { '}' };

    private static byte[] err(String message) { return Out.ascii("{\"error\":\"" + message + "\"}"); }

    /** One parsed request. {@code raw} is the header bytes the JWT is sliced from. */
    static final class Req {
        int route;
        long id;
        byte[] raw = new byte[0];
        int authFrom = -1, authTo = -1;
        int bodyFrom, bodyLen;
        boolean close;
        byte[] early;
        /** Intrusive link for the io_uring connection queue. Unused by java.nio. */
        Req next;

        /** Create and like change the database. Reads do not. */
        boolean mutates() { return route == CREATE || route == LIKE; }
    }

    private final Db db;
    private final Jwt jwt;
    private final long startNanos;
    private final Out out = new Out();

    App(Db db, Jwt jwt) {
        this.db = db;
        this.jwt = jwt;
        this.startNanos = System.nanoTime();
    }

    byte[] internalError(boolean close) {
        out.reset();
        out.close = close;
        out.respond(Out.S500, E_INTERNAL);
        return out.toByteArray();
    }

    /**
     * @return bytes consumed from {@code b[0,len)}, or {@link #INCOMPLETE} when the request is partial.
     *         A non-zero return with {@code req.early != null} is an HTTP error that did not need SQLite.
     */
    int parse(byte[] b, int len, Req req) {
        int he = headerEnd(b, len);
        if (he < 0) {
            if (len > MAX_HEAD) return fail(req, Out.S400, E_INTERNAL, len);
            return INCOMPLETE;
        }
        int sp1 = indexOf(b, (byte) ' ', 0, he);
        int sp2 = sp1 < 0 ? -1 : indexOf(b, (byte) ' ', sp1 + 1, he);
        int eol = sp2 < 0 ? -1 : indexOf(b, (byte) '\r', sp2 + 1, he);
        if (eol < 0 || eol - sp2 - 1 != 8 || !startsWith(b, sp2 + 1, "HTTP/1.")) {
            return fail(req, Out.S400, E_NOT_FOUND, Math.max(len, 1));
        }
        boolean http11 = b[sp2 + 8] == '1';
        boolean isGet = sp1 == 3 && b[0] == 'G' && b[1] == 'E' && b[2] == 'T';
        boolean isPost = sp1 == 4 && b[0] == 'P' && b[1] == 'O' && b[2] == 'S' && b[3] == 'T';
        int pathFrom = sp1 + 1;
        int pathTo = sp2;
        int q = indexOf(b, (byte) '?', pathFrom, pathTo);
        if (q >= 0) pathTo = q;

        int authFrom = -1, authTo = -1;
        long contentLength = 0;
        boolean hasLength = false, connClose = false, connKeepAlive = false;
        int line = eol + 2;
        while (line < he - 2) {
            int le = indexOf(b, (byte) '\r', line, he);
            if (le < 0) break;
            int colon = indexOf(b, (byte) ':', line, le);
            if (colon > line) {
                int vs = colon + 1, ve = le;
                while (vs < ve && (b[vs] == ' ' || b[vs] == '\t')) vs++;
                while (ve > vs && (b[ve - 1] == ' ' || b[ve - 1] == '\t')) ve--;
                int nl = colon - line;
                if (nl == 13 && nameIs(b, line, "authorization")) {
                    authFrom = vs;
                    authTo = ve;
                } else if (nl == 14 && nameIs(b, line, "content-length")) {
                    hasLength = true;
                    contentLength = 0;
                    for (int k = vs; k < ve; k++) {
                        if (b[k] < '0' || b[k] > '9') return fail(req, Out.S400, E_MALFORMED, len);
                        contentLength = contentLength * 10 + (b[k] - '0');
                        if (contentLength > MAX_BODY) return fail(req, Out.S400, E_BODY_TOO_LONG, len);
                    }
                } else if (nl == 10 && nameIs(b, line, "connection")) {
                    if (containsIgnoreCase(b, vs, ve, "close")) connClose = true;
                    if (containsIgnoreCase(b, vs, ve, "keep-alive")) connKeepAlive = true;
                }
            }
            line = le + 2;
        }
        if (contentLength > MAX_BODY) return fail(req, Out.S400, E_BODY_TOO_LONG, len);
        int msgEnd = he + (int) contentLength;
        if (!hasLength) msgEnd = he;
        if (len < msgEnd) return INCOMPLETE;

        req.route = route(b, pathFrom, pathTo, isGet, isPost);
        req.id = 0;
        req.authFrom = authFrom;
        req.authTo = authTo;
        req.bodyFrom = he;
        req.bodyLen = (int) contentLength;
        req.close = http11 ? connClose : !connKeepAlive;
        req.early = null;
        if (req.route == GET_POST) req.id = parseId(b, pathFrom + 7, pathTo);
        else if (req.route == LIKE) req.id = parseId(b, pathFrom + 7, pathTo - 5);
        if (req.route == CREATE || req.route == LIKE) {
            req.raw = new byte[msgEnd];
            System.arraycopy(b, 0, req.raw, 0, msgEnd);
        } else {
            req.raw = EMPTY_RAW;
        }
        return msgEnd;
    }

    private static final byte[] EMPTY_RAW = new byte[0];

    /** Heap response for java.nio. Call inside a transaction. */
    byte[] execute(Req req) {
        out.useHeap();
        if (req.early != null) return req.early;
        try {
            byte[] status = dispatch(req);
            if (status != null) return out.framed(status);
        } catch (RuntimeException e) {
            failInternal();
        }
        return out.toByteArray();
    }

    /**
     * Writes one full response into {@code dest}. Returns the byte count, or -1 when the
     * destination is too small (nothing in {@code dest} is then part of a response).
     * Feed and post JSON are formatted in place; only the status line slides in front.
     */
    int writeTo(Req req, MemorySegment dest) {
        out.use(dest);
        try {
            if (req.early != null) out.put(req.early);
            else {
                byte[] status = dispatch(req);
                if (status != null) out.frameInPlace(status);
            }
            return out.length();
        } catch (IndexOutOfBoundsException e) {
            return -1;
        } catch (RuntimeException e) {
            try {
                failInternal();
                return out.length();
            } catch (IndexOutOfBoundsException again) {
                return -1;
            }
        }
    }

    /** @return the status line to place in front of a body, or null when {@code out} is complete */
    private byte[] dispatch(Req req) {
        out.reset();
        out.close = req.close;
        switch (req.route) {
            case HEALTH -> { health(); return null; }
            case FEED -> { db.feed(out); return Out.S200; }
            case GET_POST -> {
                if (req.id == -1) out.respond(Out.S400, E_INVALID_ID);
                else if (req.id == -2 || !db.post(req.id, out)) out.respond(Out.S404, E_NO_POST);
                else return Out.S200;
                return null;
            }
            case CREATE -> { return create(req) ? Out.S201 : null; }
            case LIKE -> { like(req); return null; }
            default -> { out.respond(Out.S404, E_NOT_FOUND); return null; }
        }
    }

    private void failInternal() {
        System.err.println("request failed");
        out.reset();
        out.close = true;
        out.respond(Out.S500, E_INTERNAL);
    }

    private int fail(Req req, byte[] status, byte[] body, int consumed) {
        out.reset();
        out.close = true;
        out.respond(status, body);
        req.early = out.toByteArray();
        req.close = true;
        req.route = NOT_FOUND;
        return Math.max(consumed, 1);
    }

    /** @return true when the created-post JSON is in {@code out} and still needs a status line */
    private boolean create(Req req) {
        if (!auth(req)) return false;
        Object doc;
        try {
            doc = Json.parse(Json.utf8(req.raw, req.bodyFrom, req.bodyLen));
        } catch (Json.Malformed e) {
            out.respond(Out.S400, E_MALFORMED);
            return false;
        }
        if (!(Json.get(doc, "body") instanceof String raw)) {
            out.respond(Out.S400, E_BODY_REQUIRED);
            return false;
        }
        String body = trim(raw);
        if (body.isEmpty()) {
            out.respond(Out.S400, E_BODY_REQUIRED);
            return false;
        }
        if (body.codePointCount(0, body.length()) > 500) {
            out.respond(Out.S400, E_BODY_TOO_LONG);
            return false;
        }
        db.insertPost(jwt.userId, body.getBytes(StandardCharsets.UTF_8), jwt.username, out);
        return true;
    }

    private void like(Req req) {
        if (!auth(req)) return;
        if (req.id == -1) {
            out.respond(Out.S400, E_INVALID_ID);
            return;
        }
        int status = req.id == -2 ? 404 : db.like(jwt.userId, req.id);
        if (status == 404) {
            out.respond(Out.S404, E_NO_POST);
            return;
        }
        byte[] prefix = status == 201 ? LIKED_NEW : LIKED_AGAIN;
        out.head(status == 201 ? Out.S201 : Out.S200, prefix.length + Out.digits(req.id) + 1);
        out.put(prefix);
        out.putLong(req.id);
        out.put(CLOSE_BRACE);
    }

    private void health() {
        String error = db.ping();
        if (error == null) {
            long up = (System.nanoTime() - startNanos) / 1_000_000_000L;
            out.head(Out.S200, HEALTH_OK.length + Out.digits(up) + 1);
            out.put(HEALTH_OK);
            out.putLong(up);
            out.put(CLOSE_BRACE);
        } else {
            byte[] body = ("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":" + quote(error) + "}")
                    .getBytes(StandardCharsets.UTF_8);
            out.respond(Out.S503, body);
        }
    }

    private boolean auth(Req req) {
        int r = jwt.verify(req.raw, req.authFrom, req.authTo);
        if (r == Jwt.OK) return true;
        out.respond(Out.S401, r == Jwt.MISSING ? E_MISSING_TOKEN : r == Jwt.INVALID ? E_INVALID_TOKEN : E_BAD_PAYLOAD);
        return false;
    }

    private static int route(byte[] b, int pathFrom, int pathTo, boolean isGet, boolean isPost) {
        int len = pathTo - pathFrom;
        if (isGet) {
            if (len == 7 && startsWith(b, pathFrom, "/health")) return HEALTH;
            if (len == 5 && startsWith(b, pathFrom, "/feed")) return FEED;
            if (len > 7 && startsWith(b, pathFrom, "/posts/") && indexOf(b, (byte) '/', pathFrom + 7, pathTo) < 0) {
                return GET_POST;
            }
            return NOT_FOUND;
        }
        if (isPost) {
            if (len == 6 && startsWith(b, pathFrom, "/posts")) return CREATE;
            if (len > 12 && startsWith(b, pathFrom, "/posts/") && endsWith(b, pathTo, "/like")
                    && indexOf(b, (byte) '/', pathFrom + 7, pathTo - 5) < 0) {
                return LIKE;
            }
        }
        return NOT_FOUND;
    }

    private static long parseId(byte[] b, int from, int to) {
        if (from >= to) return -1;
        long v = 0;
        boolean overflow = false;
        for (int k = from; k < to; k++) {
            int d = b[k] - '0';
            if (d < 0 || d > 9) return -1;
            if (!overflow) {
                if (v > (Long.MAX_VALUE - d) / 10) overflow = true;
                else v = v * 10 + d;
            }
        }
        if (overflow) return -2;
        return v > 0 ? v : -1;
    }

    private static boolean isJsSpace(char c) {
        return c == ' ' || (c >= '\t' && c <= '\r') || c == '\u00a0' || c == '\u1680'
                || (c >= '\u2000' && c <= '\u200a') || c == '\u2028' || c == '\u2029' || c == '\u202f'
                || c == '\u205f' || c == '\u3000' || c == '\ufeff';
    }

    private static String trim(String s) {
        int a = 0, z = s.length();
        while (a < z && isJsSpace(s.charAt(a))) a++;
        while (z > a && isJsSpace(s.charAt(z - 1))) z--;
        return s.substring(a, z);
    }

    private static String quote(String s) {
        StringBuilder sb = new StringBuilder(s.length() + 2).append('"');
        for (int k = 0; k < s.length(); k++) {
            char c = s.charAt(k);
            if (c == '"' || c == '\\') sb.append('\\').append(c);
            else if (c < 0x20) sb.append(String.format("\\u%04x", (int) c));
            else sb.append(c);
        }
        return sb.append('"').toString();
    }

    private static int headerEnd(byte[] b, int end) {
        for (int k = 3; k < end; k++) {
            if (b[k] == '\n' && b[k - 1] == '\r' && b[k - 2] == '\n' && b[k - 3] == '\r') return k + 1;
        }
        return -1;
    }

    private static int indexOf(byte[] b, byte x, int from, int to) {
        for (int k = from; k < to; k++) if (b[k] == x) return k;
        return -1;
    }

    private static boolean startsWith(byte[] b, int at, String s) {
        if (at < 0 || at + s.length() > b.length) return false;
        for (int k = 0; k < s.length(); k++) if (b[at + k] != s.charAt(k)) return false;
        return true;
    }

    private static boolean endsWith(byte[] b, int end, String s) {
        int at = end - s.length();
        return startsWith(b, at, s);
    }

    private static boolean nameIs(byte[] b, int at, String lower) {
        for (int k = 0; k < lower.length(); k++) {
            int c = b[at + k];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != lower.charAt(k)) return false;
        }
        return true;
    }

    private static boolean containsIgnoreCase(byte[] b, int from, int to, String lower) {
        int n = lower.length();
        for (int i = from; i + n <= to; i++) {
            int k = 0;
            for (; k < n; k++) {
                int c = b[i + k];
                if (c >= 'A' && c <= 'Z') c += 32;
                if (c != lower.charAt(k)) break;
            }
            if (k == n) return true;
        }
        return false;
    }
}
