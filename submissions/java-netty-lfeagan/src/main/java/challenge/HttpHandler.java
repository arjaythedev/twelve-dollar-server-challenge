package challenge;

import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import io.netty.channel.ChannelFutureListener;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;

import java.nio.charset.StandardCharsets;
import java.util.Map;

/**
 * One connection: a minimal HTTP/1.1 parser (request line, Content-Length, Authorization,
 * Connection; keep-alive and pipelining) and the five routes.
 *
 * Responses produced while handling one read are gathered into a single buffer and flushed
 * once in channelReadComplete.
 */
final class HttpHandler extends ChannelInboundHandlerAdapter {
    private static final int MAX_HEADERS = 16 * 1024;
    private static final int MAX_BODY = 1024 * 1024;

    private final App app;
    private ChannelHandlerContext ctx;
    /** Unparsed bytes carried over to the next read. */
    private ByteBuf cumulation;
    /** Responses produced during the current read. */
    private ByteBuf out;
    /** Responses held back until the group commit (see App). */
    private ByteBuf held;
    boolean awaitingCommit;
    private boolean closeAfterResponses;

    // the request being handled
    private boolean keepAlive;
    private int authStart, authEnd;

    HttpHandler(App app) {
        this.app = app;
    }

    @Override
    public void handlerAdded(ChannelHandlerContext ctx) {
        this.ctx = ctx;
    }

    @Override
    public void channelRead(ChannelHandlerContext ctx, Object msg) {
        ByteBuf in = (ByteBuf) msg;
        if (closeAfterResponses) {
            in.release();
            return;
        }
        ByteBuf buf;
        if (cumulation == null) {
            buf = in;
        } else {
            cumulation.writeBytes(in);
            in.release();
            buf = cumulation;
        }
        boolean protocolError = false;
        try {
            while (!closeAfterResponses && buf.isReadable()) {
                int r = parse(buf);
                if (r == 0) break;
                if (r < 0) { protocolError = true; break; }
            }
        } catch (RuntimeException e) {
            protocolError = true;
        }

        if (protocolError || closeAfterResponses || !buf.isReadable()) {
            buf.release();
            cumulation = null;
        } else if (buf == in) {
            cumulation = ctx.alloc().heapBuffer(Math.max(1024, in.readableBytes() * 2));
            cumulation.writeBytes(in);
            in.release();
        } else {
            cumulation.discardSomeReadBytes();
        }
        if (out != null) {
            ctx.write(out, ctx.voidPromise());
            out = null;
        }
        if (protocolError) {
            closeAfterResponses = true;
            if (held != null) { held.release(); held = null; }
            ctx.writeAndFlush(Unpooled.EMPTY_BUFFER).addListener(ChannelFutureListener.CLOSE);
        }
    }

    @Override
    public void channelReadComplete(ChannelHandlerContext ctx) {
        ctx.flush();
        if (closeAfterResponses && !awaitingCommit && ctx.channel().isActive()) {
            ctx.writeAndFlush(Unpooled.EMPTY_BUFFER).addListener(ChannelFutureListener.CLOSE);
        }
    }

    /** Called by App once the group commit this connection joined has finished. */
    void committed(boolean ok) {
        awaitingCommit = false;
        ByteBuf h = held;
        held = null;
        if (!ok) {
            // never answer a write that didn't commit
            if (h != null) h.release();
            closeAfterResponses = true;
            ctx.close();
            return;
        }
        if (h != null) ctx.writeAndFlush(h, ctx.voidPromise());
        if (closeAfterResponses) ctx.writeAndFlush(Unpooled.EMPTY_BUFFER).addListener(ChannelFutureListener.CLOSE);
    }

    @Override
    public void channelInactive(ChannelHandlerContext ctx) {
        if (cumulation != null) { cumulation.release(); cumulation = null; }
        if (held != null) { held.release(); held = null; }
    }

    @Override
    public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause) {
        ctx.close();
    }

    // ---- parsing ----

    /** Handles one complete request from buf: 1 if it did, 0 if more bytes are needed, -1 on a protocol error. */
    private int parse(ByteBuf buf) {
        final int start = buf.readerIndex(), end = buf.writerIndex();
        int limit = Math.min(end, start + MAX_HEADERS);

        int lineEnd = buf.indexOf(start, limit, (byte) '\n');
        if (lineEnd < 0) return end - start >= MAX_HEADERS ? -1 : 0;
        int lineStop = trimCr(buf, start, lineEnd);
        int sp1 = buf.indexOf(start, lineStop, (byte) ' ');
        if (sp1 <= start) return -1;
        int sp2 = buf.indexOf(sp1 + 1, lineStop, (byte) ' ');
        if (sp2 < 0) return -1;
        int methodStart = start, methodLen = sp1 - start;
        int pathStart = sp1 + 1, pathEnd = sp2;
        int q = buf.indexOf(pathStart, pathEnd, (byte) '?');
        if (q >= 0) pathEnd = q;
        keepAlive = !matches(buf, sp2 + 1, lineStop - sp2 - 1, HTTP_1_0);
        authStart = -1;
        authEnd = -1;
        long contentLength = 0;

        int pos = lineEnd + 1;
        while (true) {
            lineEnd = buf.indexOf(pos, limit, (byte) '\n');
            if (lineEnd < 0) return end - start >= MAX_HEADERS ? -1 : 0;
            lineStop = trimCr(buf, pos, lineEnd);
            if (lineStop == pos) { pos = lineEnd + 1; break; }
            int colon = buf.indexOf(pos, lineStop, (byte) ':');
            if (colon <= pos) return -1;
            int vs = colon + 1, ve = lineStop;
            while (vs < ve && isSpace(buf.getByte(vs))) vs++;
            while (ve > vs && isSpace(buf.getByte(ve - 1))) ve--;
            int nameLen = colon - pos;
            if (equalsLower(buf, pos, nameLen, CONTENT_LENGTH)) {
                if (ve == vs || ve - vs > 9) return -1;
                contentLength = 0;
                for (int i = vs; i < ve; i++) {
                    byte c = buf.getByte(i);
                    if (c < '0' || c > '9') return -1;
                    contentLength = contentLength * 10 + (c - '0');
                }
            } else if (equalsLower(buf, pos, nameLen, AUTHORIZATION)) {
                authStart = vs;
                authEnd = ve;
            } else if (equalsLower(buf, pos, nameLen, CONNECTION)) {
                if (equalsLower(buf, vs, ve - vs, CLOSE)) keepAlive = false;
                else if (equalsLower(buf, vs, ve - vs, KEEP_ALIVE)) keepAlive = true;
            } else if (equalsLower(buf, pos, nameLen, TRANSFER_ENCODING)) {
                return -1; // chunked request bodies are not supported
            }
            pos = lineEnd + 1;
        }

        if (contentLength > MAX_BODY) return -1;
        int bodyStart = pos, bodyLen = (int) contentLength;
        if (end - bodyStart < bodyLen) return 0;

        if (!keepAlive) closeAfterResponses = true;
        try {
            route(buf, methodStart, methodLen, pathStart, pathEnd, bodyStart, bodyLen);
        } catch (RuntimeException e) {
            respond(500, INTERNAL_ERROR);
        }
        buf.readerIndex(bodyStart + bodyLen);
        return 1;
    }

    private void route(ByteBuf buf, int ms, int ml, int ps, int pe, int bs, int bl) {
        int pl = pe - ps;
        if (matches(buf, ms, ml, GET)) {
            if (matches(buf, ps, pl, FEED)) { feed(); return; }
            if (matches(buf, ps, pl, HEALTH)) { health(); return; }
            if (pl > POSTS_SLASH.length && matches(buf, ps, POSTS_SLASH.length, POSTS_SLASH)
                    && buf.indexOf(ps + POSTS_SLASH.length, pe, (byte) '/') < 0) {
                getPost(parseId(buf, ps + POSTS_SLASH.length, pe));
                return;
            }
        } else if (matches(buf, ms, ml, POST)) {
            if (matches(buf, ps, pl, POSTS)) { createPost(buf, bs, bl); return; }
            if (pl >= POSTS_SLASH.length + LIKE.length && matches(buf, ps, POSTS_SLASH.length, POSTS_SLASH)
                    && matches(buf, pe - LIKE.length, LIKE.length, LIKE)) {
                int idStart = ps + POSTS_SLASH.length, idEnd = pe - LIKE.length;
                if (buf.indexOf(idStart, idEnd, (byte) '/') < 0) {
                    like(buf, idStart, idEnd);
                    return;
                }
            }
        }
        respond(404, NOT_FOUND);
    }

    // ---- routes ----

    private void health() {
        ByteBuf b = body();
        try {
            app.db.health();
        } catch (RuntimeException e) {
            b.writeBytes(DEGRADED);
            Json.writeEscaped(b, String.valueOf(e.getMessage()));
            b.writeBytes(QUOTE_BRACE);
            respond(503, b);
            return;
        }
        b.writeBytes(HEALTH_OK);
        Json.writeLong(b, (System.nanoTime() - app.startNanos) / 1_000_000_000L);
        b.writeByte('}');
        respond(200, b);
    }

    private void feed() {
        ByteBuf b = body();
        app.db.writeFeed(b);
        respond(200, b);
    }

    private void getPost(long id) {
        if (id < 0) { respond(400, INVALID_ID); return; }
        ByteBuf b = body();
        if (app.db.writePostById(id, b)) respond(200, b);
        else respond(404, POST_NOT_FOUND);
    }

    private void createPost(ByteBuf buf, int bs, int bl) {
        if (!authenticate(buf)) return;
        byte[] raw = bl <= app.request.length ? app.request : new byte[bl];
        buf.getBytes(bs, raw, 0, bl);
        Object doc;
        try {
            doc = Json.parse(raw, 0, bl);
        } catch (Json.ParseException e) {
            respond(400, MALFORMED_JSON);
            return;
        }
        if (!(doc instanceof Map<?, ?> m) || !(m.get("body") instanceof String text)) {
            respond(400, BODY_REQUIRED);
            return;
        }
        String trimmed = text.strip();
        if (trimmed.isEmpty()) { respond(400, BODY_REQUIRED); return; }
        if (trimmed.codePointCount(0, trimmed.length()) > 500) { respond(400, BODY_TOO_LONG); return; }

        long userId = app.jwt.userId;
        String username = app.jwt.username;
        app.joinWrite(this);
        ByteBuf b = body();
        app.db.createPost(userId, trimmed.getBytes(StandardCharsets.UTF_8), username, b);
        respond(201, b);
    }

    private void like(ByteBuf buf, int idStart, int idEnd) {
        if (!authenticate(buf)) return;
        long id = parseId(buf, idStart, idEnd);
        if (id < 0) { respond(400, INVALID_ID); return; }
        long userId = app.jwt.userId;
        app.joinWrite(this);
        int result = app.db.like(userId, id);
        if (result == Db.NO_SUCH_POST) { respond(404, POST_NOT_FOUND); return; }
        ByteBuf b = body();
        b.writeBytes(result == Db.LIKED ? LIKED : ALREADY_LIKED);
        Json.writeLong(b, id);
        b.writeByte('}');
        respond(result, b);
    }

    private boolean authenticate(ByteBuf buf) {
        if (authStart < 0 || !matches(buf, authStart, Math.min(BEARER.length, authEnd - authStart), BEARER)) {
            respond(401, MISSING_TOKEN);
            return false;
        }
        int r = app.jwt.verify(buf, authStart + BEARER.length, authEnd);
        if (r == Jwt.OK) return true;
        respond(401, r == Jwt.BAD_PAYLOAD ? BAD_PAYLOAD : INVALID_TOKEN);
        return false;
    }

    /** A positive decimal id of at most 18 digits, or -1. */
    private static long parseId(ByteBuf buf, int from, int to) {
        int len = to - from;
        if (len <= 0 || len > 18) return -1;
        long v = 0;
        for (int i = from; i < to; i++) {
            byte c = buf.getByte(i);
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v > 0 ? v : -1;
    }

    // ---- responses ----

    private ByteBuf body() {
        return app.body.clear();
    }

    private void respond(int status, byte[] fixedBody) {
        ByteBuf o = target(fixedBody.length);
        writeHead(o, status, fixedBody.length);
        o.writeBytes(fixedBody);
    }

    private void respond(int status, ByteBuf b) {
        int len = b.readableBytes();
        ByteBuf o = target(len);
        writeHead(o, status, len);
        o.writeBytes(b, b.readerIndex(), len);
    }

    private ByteBuf target(int bodyLen) {
        if (awaitingCommit) {
            if (held == null) held = ctx.alloc().ioBuffer(bodyLen + 128);
            return held;
        }
        if (out == null) out = ctx.alloc().ioBuffer(bodyLen + 128);
        return out;
    }

    private void writeHead(ByteBuf o, int status, int len) {
        o.writeBytes(statusLine(status));
        o.writeBytes(HEADERS);
        Json.writeLong(o, len);
        o.writeBytes(keepAlive ? END_HEADERS : END_HEADERS_CLOSE);
    }

    private static byte[] statusLine(int status) {
        return switch (status) {
            case 200 -> S200;
            case 201 -> S201;
            case 400 -> S400;
            case 401 -> S401;
            case 404 -> S404;
            case 503 -> S503;
            default -> S500;
        };
    }

    // ---- byte helpers ----

    private static int trimCr(ByteBuf buf, int from, int lf) {
        return lf > from && buf.getByte(lf - 1) == '\r' ? lf - 1 : lf;
    }

    private static boolean isSpace(byte c) {
        return c == ' ' || c == '\t';
    }

    private static boolean matches(ByteBuf buf, int at, int len, byte[] s) {
        if (len != s.length) return false;
        for (int i = 0; i < len; i++) if (buf.getByte(at + i) != s[i]) return false;
        return true;
    }

    /** Case-insensitive match against a lower-case ASCII name. */
    private static boolean equalsLower(ByteBuf buf, int at, int len, byte[] lower) {
        if (len != lower.length) return false;
        for (int i = 0; i < len; i++) if ((buf.getByte(at + i) | 0x20) != lower[i]) return false;
        return true;
    }

    private static byte[] ascii(String s) { return s.getBytes(StandardCharsets.US_ASCII); }

    private static final byte[] GET = ascii("GET"), POST = ascii("POST");
    private static final byte[] FEED = ascii("/feed"), HEALTH = ascii("/health"), POSTS = ascii("/posts");
    private static final byte[] POSTS_SLASH = ascii("/posts/"), LIKE = ascii("/like");
    private static final byte[] HTTP_1_0 = ascii("HTTP/1.0");
    private static final byte[] CONTENT_LENGTH = ascii("content-length"), AUTHORIZATION = ascii("authorization");
    private static final byte[] CONNECTION = ascii("connection"), TRANSFER_ENCODING = ascii("transfer-encoding");
    private static final byte[] CLOSE = ascii("close"), KEEP_ALIVE = ascii("keep-alive");
    private static final byte[] BEARER = ascii("Bearer ");

    private static final byte[] S200 = ascii("HTTP/1.1 200 OK\r\n");
    private static final byte[] S201 = ascii("HTTP/1.1 201 Created\r\n");
    private static final byte[] S400 = ascii("HTTP/1.1 400 Bad Request\r\n");
    private static final byte[] S401 = ascii("HTTP/1.1 401 Unauthorized\r\n");
    private static final byte[] S404 = ascii("HTTP/1.1 404 Not Found\r\n");
    private static final byte[] S500 = ascii("HTTP/1.1 500 Internal Server Error\r\n");
    private static final byte[] S503 = ascii("HTTP/1.1 503 Service Unavailable\r\n");
    private static final byte[] HEADERS = ascii("Content-Type: application/json\r\nContent-Length: ");
    private static final byte[] END_HEADERS = ascii("\r\n\r\n");
    private static final byte[] END_HEADERS_CLOSE = ascii("\r\nConnection: close\r\n\r\n");

    private static final byte[] HEALTH_OK = ascii("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    private static final byte[] DEGRADED = ascii("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
    private static final byte[] QUOTE_BRACE = ascii("\"}");
    private static final byte[] LIKED = ascii("{\"liked\":true,\"already_liked\":false,\"post_id\":");
    private static final byte[] ALREADY_LIKED = ascii("{\"liked\":true,\"already_liked\":true,\"post_id\":");

    private static byte[] error(String message) { return ascii("{\"error\":\"" + message + "\"}"); }

    private static final byte[] NOT_FOUND = error("not found");
    private static final byte[] INVALID_ID = error("invalid post id");
    private static final byte[] POST_NOT_FOUND = error("post not found");
    private static final byte[] MISSING_TOKEN = error("missing bearer token");
    private static final byte[] INVALID_TOKEN = error("invalid or expired token");
    private static final byte[] BAD_PAYLOAD = error("invalid token payload");
    private static final byte[] MALFORMED_JSON = error("malformed JSON body");
    private static final byte[] BODY_REQUIRED = error("body is required");
    private static final byte[] BODY_TOO_LONG = error("body must be at most 500 characters");
    private static final byte[] INTERNAL_ERROR = error("internal server error");
}
