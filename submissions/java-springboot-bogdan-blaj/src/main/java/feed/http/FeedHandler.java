package feed.http;

import feed.auth.JwtVerifier;
import feed.db.FeedDb;
import feed.json.BodyParser;
import io.netty.buffer.ByteBuf;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import io.netty.handler.codec.http.HttpContent;
import io.netty.handler.codec.http.HttpHeaderNames;
import io.netty.handler.codec.http.HttpMethod;
import io.netty.handler.codec.http.HttpRequest;
import io.netty.handler.codec.http.HttpUtil;
import io.netty.handler.codec.http.LastHttpContent;
import io.netty.util.ReferenceCountUtil;

/**
 * One per connection (not sharable), after {@code HttpRequestDecoder}. Frames requests, routes them, runs them
 * against SQLite and hands complete responses to {@link Batch}. Kept small: there may be 64k of these.
 *
 * <p>Every message from the decoder is released on every path. A request body is accumulated in a pooled buffer
 * only while it arrives in several pieces; the common case (whole body in the last content) is parsed in place.
 */
final class FeedHandler extends ChannelInboundHandlerAdapter {

    static final int MAX_BODY = 16 * 1024;

    static final int R_NONE = 0, R_HEALTH = 1, R_FEED = 2, R_POST = 3, R_CREATE = 4, R_LIKE = 5, R_NOT_FOUND = 6,
            R_BAD_ID = 7;
    private static final long MAX_ID_DIGITS = 15;
    private static final byte[] EMPTY = new byte[0];

    private final FeedContext app;
    private ChannelHandlerContext ctx;

    // Owned by Connections.
    FeedHandler prev;
    FeedHandler next;
    int lastActiveSec;
    boolean linked;
    boolean counted;

    // Current request.
    private byte route;
    private boolean keepAlive;
    /** A response that closes the connection was produced; everything after it is ignored. */
    private boolean closing;
    private long id;
    private String auth;
    private ByteBuf body;

    FeedHandler(FeedContext app) {
        this.app = app;
    }

    @Override
    public void handlerAdded(ChannelHandlerContext ctx) {
        this.ctx = ctx;
    }

    @Override
    public void channelActive(ChannelHandlerContext ctx) {
        app.connections.add(this);
    }

    @Override
    public void channelInactive(ChannelHandlerContext ctx) {
        closing = true;
        route = R_NONE;
        auth = null;
        releaseBody();
        app.connections.remove(this);
    }

    @Override
    public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause) {
        ctx.close();
    }

    @Override
    public void channelRead(ChannelHandlerContext ctx, Object msg) {
        try {
            if (closing) {
                return;
            }
            if (msg instanceof HttpRequest req) {
                onRequest(req);
            }
            if (msg instanceof HttpContent content && !closing) {
                onContent(content);
            }
        } finally {
            ReferenceCountUtil.release(msg);
        }
    }

    /** Called by the idle sweep, which has already unlinked this handler. */
    void closeInactive() {
        ctx.close();
    }

    // ---- framing

    private void onRequest(HttpRequest req) {
        if (req.decoderResult().isFailure()) {
            fail(Responses.BAD_REQUEST);
            return;
        }
        app.connections.touch(this);
        keepAlive = HttpUtil.isKeepAlive(req);
        long r = parseRoute(req.method(), req.uri());
        route = (byte) r;
        id = (r >>> 8) - 1;
        if (route == R_CREATE || route == R_LIKE) {
            auth = req.headers().get(HttpHeaderNames.AUTHORIZATION);
            if (route == R_CREATE) {
                long length;
                try {
                    length = HttpUtil.getContentLength(req, -1L);
                } catch (NumberFormatException e) {
                    fail(Responses.BAD_REQUEST);
                    return;
                }
                if (length > MAX_BODY) {
                    fail(Responses.PAYLOAD_TOO_LARGE);
                    return;
                }
            }
        }
        if (HttpUtil.is100ContinueExpected(req)) {
            // Through the batch, so it cannot overtake a held response of an earlier pipelined request.
            app.batch.respond(ctx, Responses.continue100(), false);
        }
    }

    private void onContent(HttpContent content) {
        if (content.decoderResult().isFailure()) {
            fail(Responses.BAD_REQUEST);
            return;
        }
        if (route == R_NONE) {
            return;
        }
        boolean last = content instanceof LastHttpContent;
        if (route == R_CREATE) {
            ByteBuf data = content.content();
            int n = data.readableBytes();
            if (body == null && last) {
                if (n > MAX_BODY) {
                    fail(Responses.PAYLOAD_TOO_LARGE);
                } else {
                    dispatch(data);
                }
                return;
            }
            if (n > 0) {
                ByteBuf b = body;
                if (b == null) {
                    b = body = ctx.alloc().heapBuffer(Math.min(Math.max(n * 2, 1024), MAX_BODY), MAX_BODY);
                }
                if (b.readableBytes() + n > MAX_BODY) {
                    fail(Responses.PAYLOAD_TOO_LARGE);
                    return;
                }
                b.writeBytes(data);
            }
            if (last) {
                dispatch(body);
            }
        } else if (last) {
            dispatch(null);
        }
    }

    private void fail(int fixedResponse) {
        route = R_NONE;
        auth = null;
        releaseBody();
        closing = true;
        app.batch.respond(ctx, Responses.fixed(fixedResponse, true), true);
    }

    private void releaseBody() {
        ByteBuf b = body;
        if (b != null) {
            body = null;
            b.release();
        }
    }

    // ---- dispatch

    private void dispatch(ByteBuf content) {
        int r = route;
        String a = auth;
        route = R_NONE;
        auth = null;
        if (!ctx.channel().isActive()) {
            releaseBody();
            return;
        }
        boolean close = !keepAlive;
        if (!close && app.connections.overLimit()) {
            close = true;
            app.connections.countLimitClose();
        }
        Batch batch = app.batch;
        batch.noteRequest();
        ByteBuf out;
        try {
            out = switch (r) {
                case R_FEED -> feed(close);
                case R_POST -> post(close);
                case R_HEALTH -> health(close);
                case R_CREATE -> create(a, content, close);
                case R_LIKE -> like(a, close);
                case R_BAD_ID -> Responses.fixed(Responses.INVALID_POST_ID, close);
                default -> Responses.fixed(Responses.NOT_FOUND, close);
            };
        } finally {
            releaseBody();
        }
        if (close) {
            closing = true;
        }
        batch.respond(ctx, out, close);
    }

    private ByteBuf feed(boolean close) {
        FeedContext a = app;
        if (!a.batch.begin()) {
            return Responses.fixed(Responses.INTERNAL_ERROR, close);
        }
        ResponseSink s = a.sink.prepare(ctx.alloc(), Responses.S200, close);
        try {
            a.db.feed(s);
            return orInternalError(s.take(), close);
        } catch (RuntimeException e) {
            s.discard();
            a.dbError("feed", e);
            return Responses.fixed(Responses.INTERNAL_ERROR, close);
        }
    }

    private ByteBuf post(boolean close) {
        FeedContext a = app;
        if (!a.batch.begin()) {
            return Responses.fixed(Responses.INTERNAL_ERROR, close);
        }
        ResponseSink s = a.sink.prepare(ctx.alloc(), Responses.S200, close);
        try {
            if (a.db.post(id, s)) {
                return orInternalError(s.take(), close);
            }
            s.discard();
            return Responses.fixed(Responses.POST_NOT_FOUND, close);
        } catch (RuntimeException e) {
            s.discard();
            a.dbError("post", e);
            return Responses.fixed(Responses.INTERNAL_ERROR, close);
        }
    }

    private ByteBuf health(boolean close) {
        FeedContext a = app;
        String err;
        if (!a.batch.begin()) {
            err = "BEGIN failed";
        } else {
            try {
                err = a.db.health();
            } catch (RuntimeException e) {
                a.dbError("health", e);
                err = e.getMessage() == null ? e.toString() : e.getMessage();
            }
        }
        if (err == null) {
            return Responses.healthOk(ctx.alloc(), close, (System.currentTimeMillis() - a.startMillis) / 1000);
        }
        return Responses.healthDegraded(ctx.alloc(), close, err);
    }

    private ByteBuf create(String authorization, ByteBuf content, boolean close) {
        FeedContext a = app;
        JwtVerifier jwt = a.jwt;
        int v = jwt.verify(authorization, System.currentTimeMillis() / 1000);
        if (v != JwtVerifier.OK) {
            return Responses.fixed(Responses.authError(v), close);
        }
        BodyParser parser = a.parser;
        int n = content == null ? 0 : content.readableBytes();
        int p;
        if (n == 0) {
            p = parser.parse(EMPTY, 0, 0);
        } else if (content.hasArray()) {
            p = parser.parse(content.array(), content.arrayOffset() + content.readerIndex(), n);
        } else {
            content.getBytes(content.readerIndex(), a.bodyScratch, 0, n);
            p = parser.parse(a.bodyScratch, 0, n);
        }
        if (p != BodyParser.OK) {
            return Responses.fixed(Responses.bodyError(p), close);
        }
        if (!a.batch.begin()) {
            return Responses.fixed(Responses.INTERNAL_ERROR, close);
        }
        ResponseSink s = a.sink.prepare(ctx.alloc(), Responses.S201, close);
        try {
            a.db.create(jwt.userId(), parser.body(), parser.bodyLength(), jwt.username(), jwt.usernameLength(), s);
            return orInternalError(s.take(), close);
        } catch (RuntimeException e) {
            // The sink may have run before the statement failed: that response must never leave.
            s.discard();
            a.dbError("create", e);
            return Responses.fixed(Responses.INTERNAL_ERROR, close);
        }
    }

    private ByteBuf like(String authorization, boolean close) {
        FeedContext a = app;
        JwtVerifier jwt = a.jwt;
        int v = jwt.verify(authorization, System.currentTimeMillis() / 1000);
        if (v != JwtVerifier.OK) {
            return Responses.fixed(Responses.authError(v), close);
        }
        long postId = id;
        if (postId < 1) {
            return Responses.fixed(Responses.INVALID_POST_ID, close);
        }
        if (!a.batch.begin()) {
            return Responses.fixed(Responses.INTERNAL_ERROR, close);
        }
        int result;
        try {
            result = a.db.like(jwt.userId(), postId);
        } catch (RuntimeException e) {
            a.dbError("like", e);
            return Responses.fixed(Responses.INTERNAL_ERROR, close);
        }
        return switch (result) {
            case FeedDb.LIKE_CREATED -> Responses.like(ctx.alloc(), true, close, postId);
            case FeedDb.LIKE_REPEAT -> Responses.like(ctx.alloc(), false, close, postId);
            default -> Responses.fixed(Responses.POST_NOT_FOUND, close);
        };
    }

    private static ByteBuf orInternalError(ByteBuf b, boolean close) {
        return b != null ? b : Responses.fixed(Responses.INTERNAL_ERROR, close);
    }

    // ---- routing

    /**
     * Route of a request line, packed as {@code ((id + 1) << 8) | route}; id is -1 when absent or invalid.
     * The query string is ignored. Paths match exactly (no trailing slash). {@code GET /posts/<x>} with an invalid
     * id gives {@link #R_BAD_ID}; {@code POST /posts/<x>/like} keeps id -1 so that auth is checked first.
     */
    static long parseRoute(HttpMethod method, String uri) {
        int end = uri.indexOf('?');
        if (end < 0) {
            end = uri.length();
        }
        if (HttpMethod.GET.equals(method)) {
            if (end == 5 && uri.startsWith("/feed")) {
                return R_FEED;
            }
            if (end == 7 && uri.startsWith("/health")) {
                return R_HEALTH;
            }
            if (end > 7 && uri.startsWith("/posts/")) {
                if (hasSlash(uri, 7, end)) {
                    return R_NOT_FOUND;
                }
                long postId = parseId(uri, 7, end);
                return postId > 0 ? pack(R_POST, postId) : R_BAD_ID;
            }
        } else if (HttpMethod.POST.equals(method)) {
            if (end == 6 && uri.startsWith("/posts")) {
                return R_CREATE;
            }
            if (end > 12 && uri.startsWith("/posts/") && uri.startsWith("/like", end - 5)) {
                if (hasSlash(uri, 7, end - 5)) {
                    return R_NOT_FOUND;
                }
                return pack(R_LIKE, parseId(uri, 7, end - 5));
            }
        }
        return R_NOT_FOUND;
    }

    private static long pack(int route, long id) {
        return ((id + 1) << 8) | route;
    }

    private static boolean hasSlash(String s, int from, int to) {
        for (int i = from; i < to; i++) {
            if (s.charAt(i) == '/') {
                return true;
            }
        }
        return false;
    }

    /** {@code [0-9]{1,15}} with value at least 1 (leading zeros allowed), else -1. */
    static long parseId(CharSequence s, int from, int to) {
        int n = to - from;
        if (n < 1 || n > MAX_ID_DIGITS) {
            return -1;
        }
        long v = 0;
        for (int i = from; i < to; i++) {
            int d = s.charAt(i) - '0';
            if (d < 0 || d > 9) {
                return -1;
            }
            v = v * 10 + d;
        }
        return v >= 1 ? v : -1;
    }
}
