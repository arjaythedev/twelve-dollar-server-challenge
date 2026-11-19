package app;

import java.nio.charset.StandardCharsets;

/** Every constant byte sequence the server can send: status/header blocks, error bodies, JSON frames. */
final class Responses {
    private Responses() {}

    // Status indexes. A handler returns one of these; Connection turns it into a header block.
    static final int S200 = 0, S201 = 1, S400 = 2, S401 = 3, S404 = 4, S500 = 5, S503 = 6;

    private static final String[] STATUS_LINES = {
        "200 OK", "201 Created", "400 Bad Request", "401 Unauthorized",
        "404 Not Found", "500 Internal Server Error", "503 Service Unavailable",
    };

    /** Header block up to and including "Content-Length: "; the digits and CRLFCRLF follow. */
    static final byte[][] HDR_KEEP = new byte[STATUS_LINES.length][];
    static final byte[][] HDR_CLOSE = new byte[STATUS_LINES.length][];
    /** HTTP/1.0 defaults to closing, so a 1.0 client that asked to keep the connection is told. */
    static final byte[][] HDR_KEEP_10 = new byte[STATUS_LINES.length][];
    static {
        for (int i = 0; i < STATUS_LINES.length; i++) {
            HDR_KEEP[i] = header(STATUS_LINES[i], "");
            HDR_CLOSE[i] = header(STATUS_LINES[i], "Connection: close\r\n");
            HDR_KEEP_10[i] = header(STATUS_LINES[i], "Connection: keep-alive\r\n");
        }
    }
    private static byte[] header(String statusLine, String connection) {
        return b("HTTP/1.1 " + statusLine + "\r\nContent-Type: application/json\r\n"
                + connection + "Content-Length: ");
    }

    static final byte[] CONTINUE = b("HTTP/1.1 100 Continue\r\n\r\n");

    static final byte[] ERR_NOT_FOUND      = b("{\"error\":\"not found\"}");
    static final byte[] ERR_INVALID_ID     = b("{\"error\":\"invalid post id\"}");
    static final byte[] ERR_POST_NOT_FOUND = b("{\"error\":\"post not found\"}");
    static final byte[] ERR_NO_BEARER      = b("{\"error\":\"missing bearer token\"}");
    static final byte[] ERR_BAD_TOKEN      = b("{\"error\":\"invalid or expired token\"}");
    static final byte[] ERR_BAD_PAYLOAD    = b("{\"error\":\"invalid token payload\"}");
    static final byte[] ERR_BAD_JSON       = b("{\"error\":\"malformed JSON body\"}");
    static final byte[] ERR_BODY_REQUIRED  = b("{\"error\":\"body is required\"}");
    static final byte[] ERR_BODY_TOO_LONG  = b("{\"error\":\"body must be at most 500 characters\"}");
    static final byte[] ERR_INTERNAL       = b("{\"error\":\"internal server error\"}");
    static final byte[] ERR_BAD_REQUEST    = b("{\"error\":\"bad request\"}");

    static final byte[] FEED_OPEN  = b("{\"posts\":[");
    static final byte[] FEED_CLOSE = b("]}");
    static final byte[] POST_OPEN  = b("{\"post\":");
    static final byte[] HEALTH_OK  = b("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    static final byte[] HEALTH_BAD = b("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database read failed\"}");
    static final byte[] LIKE_OPEN  = b("{\"liked\":true,\"already_liked\":");
    static final byte[] LIKE_MID   = b(",\"post_id\":");
    static final byte[] TRUE       = b("true");
    static final byte[] FALSE      = b("false");

    private static byte[] b(String s) { return s.getBytes(StandardCharsets.ISO_8859_1); }
}
