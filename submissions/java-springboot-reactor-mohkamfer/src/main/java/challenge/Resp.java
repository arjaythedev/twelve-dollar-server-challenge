package challenge;

import java.nio.charset.StandardCharsets;

/** A finished response: status code plus the exact JSON bytes. */
record Resp(int status, byte[] body) {

    static final Resp NOT_FOUND = error(404, "not found");
    static final Resp INVALID_POST_ID = error(400, "invalid post id");
    static final Resp POST_NOT_FOUND = error(404, "post not found");
    static final Resp MISSING_TOKEN = error(401, "missing bearer token");
    static final Resp INVALID_TOKEN = error(401, "invalid or expired token");
    static final Resp INVALID_PAYLOAD = error(401, "invalid token payload");
    static final Resp MALFORMED_JSON = error(400, "malformed JSON body");
    static final Resp BODY_REQUIRED = error(400, "body is required");
    static final Resp BODY_TOO_LONG = error(400, "body must be at most 500 characters");
    static final Resp INTERNAL_ERROR = error(500, "internal server error");

    static Resp error(int status, String message) {
        return new Resp(status, ("{\"error\":\"" + message + "\"}").getBytes(StandardCharsets.UTF_8));
    }
}
