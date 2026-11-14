package server;

import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import io.netty.channel.ChannelFutureListener;
import io.netty.channel.ChannelHandler;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.SimpleChannelInboundHandler;
import io.netty.handler.codec.http.DefaultFullHttpResponse;
import io.netty.handler.codec.http.FullHttpRequest;
import io.netty.handler.codec.http.FullHttpResponse;
import io.netty.handler.codec.http.HttpHeaderNames;
import io.netty.handler.codec.http.HttpHeaderValues;
import io.netty.handler.codec.http.HttpMethod;
import io.netty.handler.codec.http.HttpResponseStatus;
import io.netty.handler.codec.http.HttpUtil;
import io.netty.handler.codec.http.HttpVersion;
import io.netty.util.AsciiString;
import server.db.Database;
import server.json.FastJsonWriter;
import server.util.BodyParser;
import server.util.FastJwt;
import server.util.StringUtil;

import java.nio.charset.StandardCharsets;

@ChannelHandler.Sharable
public final class RequestHandler extends SimpleChannelInboundHandler<FullHttpRequest> {

    private final Database db;
    private final FastJwt jwt;
    private final long startTime;

    private static final AsciiString APPLICATION_JSON = HttpHeaderValues.APPLICATION_JSON;

    // Pre-encoded static error response bodies
    private static final byte[] ERR_NOT_FOUND_BYTES = "{\"error\":\"not found\"}".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] ERR_INVALID_POST_ID_BYTES = "{\"error\":\"invalid post id\"}".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] ERR_POST_NOT_FOUND_BYTES = "{\"error\":\"post not found\"}".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] ERR_INTERNAL_SERVER_ERROR_BYTES = "{\"error\":\"internal server error\"}".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] ERR_MALFORMED_JSON_BYTES = "{\"error\":\"malformed JSON body\"}".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] ERR_BODY_REQUIRED_BYTES = "{\"error\":\"body is required\"}".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] ERR_BODY_TOO_LONG_BYTES = "{\"error\":\"body must be at most 500 characters\"}".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] ERR_DB_UNHEALTHY_BYTES = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database unhealthy\"}".getBytes(StandardCharsets.US_ASCII);

    public RequestHandler(Database db, FastJwt jwt, long startTime) {
        this.db = db;
        this.jwt = jwt;
        this.startTime = startTime;
    }

    @Override
    protected void channelRead0(ChannelHandlerContext ctx, FullHttpRequest req) {
        String uri = req.uri();
        HttpMethod method = req.method();

        try {
            // Hot path 1: GET /feed (~46% of all traffic)
            if ("/feed".equals(uri)) {
                handleFeed(ctx, req, method);
                return;
            }

            // Hot path 2: /posts/... (~46% GET /posts/:id, ~7% POST /posts/:id/like)
            if (uri.startsWith("/posts/")) {
                handlePostsParameterized(ctx, req, method, uri);
                return;
            }

            // Write path: POST /posts (~1% of traffic)
            if ("/posts".equals(uri)) {
                handleCreatePost(ctx, req, method);
                return;
            }

            // Health check (<0.01% of traffic)
            if ("/health".equals(uri)) {
                handleHealth(ctx, req, method);
                return;
            }

            sendStaticError(ctx, req, HttpResponseStatus.NOT_FOUND, ERR_NOT_FOUND_BYTES);
        } catch (Exception e) {
            sendStaticError(ctx, req, HttpResponseStatus.INTERNAL_SERVER_ERROR, ERR_INTERNAL_SERVER_ERROR_BYTES);
        }
    }

    @Override
    public void channelReadComplete(ChannelHandlerContext ctx) {
        ctx.flush();
    }

    private void handleFeed(ChannelHandlerContext ctx, FullHttpRequest req, HttpMethod method) throws Exception {
        if (method != HttpMethod.GET) {
            sendStaticError(ctx, req, HttpResponseStatus.NOT_FOUND, ERR_NOT_FOUND_BYTES);
            return;
        }

        ByteBuf buf = ctx.alloc().buffer(8192);
        db.writeFeedJson(buf);
        sendResponse(ctx, req, HttpResponseStatus.OK, buf);
    }

    private void handlePostsParameterized(ChannelHandlerContext ctx, FullHttpRequest req, HttpMethod method, String uri) throws Exception {
        int slashIndex = uri.indexOf('/', 7);
        if (slashIndex >= 0) {
            // Check if subpath is exactly "/like"
            int uriLen = uri.length();
            if (slashIndex + 5 == uriLen
                    && uri.charAt(slashIndex + 1) == 'l'
                    && uri.charAt(slashIndex + 2) == 'i'
                    && uri.charAt(slashIndex + 3) == 'k'
                    && uri.charAt(slashIndex + 4) == 'e') {

                // POST /posts/:id/like
                if (method != HttpMethod.POST) {
                    sendStaticError(ctx, req, HttpResponseStatus.NOT_FOUND, ERR_NOT_FOUND_BYTES);
                    return;
                }

                // SPEC Check 22: Auth FIRST before ID!
                FastJwt.AuthResult auth = jwt.authenticate(req.headers().get(HttpHeaderNames.AUTHORIZATION));
                if (auth instanceof FastJwt.AuthResult.Failure f) {
                    sendError(ctx, req, HttpResponseStatus.valueOf(f.statusCode()), f.errorMessage());
                    return;
                }
                FastJwt.AuthResult.Success user = (FastJwt.AuthResult.Success) auth;

                long postId = StringUtil.parsePositiveId(uri, 7, slashIndex);
                if (postId <= 0) {
                    sendStaticError(ctx, req, HttpResponseStatus.BAD_REQUEST, ERR_INVALID_POST_ID_BYTES);
                    return;
                }

                Database.LikeResult likeRes = db.likePost(user.userId(), postId);
                if (likeRes == Database.LikeResult.CREATED) {
                    ByteBuf buf = ctx.alloc().buffer(64);
                    FastJsonWriter.writeLikeResponse(buf, false, postId);
                    sendResponse(ctx, req, HttpResponseStatus.CREATED, buf);
                } else if (likeRes == Database.LikeResult.ALREADY_LIKED) {
                    ByteBuf buf = ctx.alloc().buffer(64);
                    FastJsonWriter.writeLikeResponse(buf, true, postId);
                    sendResponse(ctx, req, HttpResponseStatus.OK, buf);
                } else {
                    sendStaticError(ctx, req, HttpResponseStatus.NOT_FOUND, ERR_POST_NOT_FOUND_BYTES);
                }
            } else {
                // Unknown path after /posts/
                sendStaticError(ctx, req, HttpResponseStatus.NOT_FOUND, ERR_NOT_FOUND_BYTES);
            }
        } else {
            // GET /posts/:id
            if (method != HttpMethod.GET) {
                sendStaticError(ctx, req, HttpResponseStatus.NOT_FOUND, ERR_NOT_FOUND_BYTES);
                return;
            }

            long postId = StringUtil.parsePositiveId(uri, 7, uri.length());
            if (postId <= 0) {
                sendStaticError(ctx, req, HttpResponseStatus.BAD_REQUEST, ERR_INVALID_POST_ID_BYTES);
                return;
            }

            ByteBuf buf = ctx.alloc().buffer(1024);
            if (!db.writePostByIdJson(buf, postId)) {
                buf.release();
                sendStaticError(ctx, req, HttpResponseStatus.NOT_FOUND, ERR_POST_NOT_FOUND_BYTES);
                return;
            }

            sendResponse(ctx, req, HttpResponseStatus.OK, buf);
        }
    }

    private void handleCreatePost(ChannelHandlerContext ctx, FullHttpRequest req, HttpMethod method) {
        if (method != HttpMethod.POST) {
            sendStaticError(ctx, req, HttpResponseStatus.NOT_FOUND, ERR_NOT_FOUND_BYTES);
            return;
        }

        FastJwt.AuthResult auth = jwt.authenticate(req.headers().get(HttpHeaderNames.AUTHORIZATION));
        if (auth instanceof FastJwt.AuthResult.Failure f) {
            sendError(ctx, req, HttpResponseStatus.valueOf(f.statusCode()), f.errorMessage());
            return;
        }
        FastJwt.AuthResult.Success user = (FastJwt.AuthResult.Success) auth;

        // Parse Body
        String content = req.content().toString(StandardCharsets.UTF_8);
        BodyParser.ParseResult parseRes = BodyParser.parsePostBody(content);
        if (parseRes instanceof BodyParser.ParseResult.MalformedJson) {
            sendStaticError(ctx, req, HttpResponseStatus.BAD_REQUEST, ERR_MALFORMED_JSON_BYTES);
            return;
        }
        if (parseRes instanceof BodyParser.ParseResult.BodyRequired) {
            sendStaticError(ctx, req, HttpResponseStatus.BAD_REQUEST, ERR_BODY_REQUIRED_BYTES);
            return;
        }
        if (parseRes instanceof BodyParser.ParseResult.BodyTooLong) {
            sendStaticError(ctx, req, HttpResponseStatus.BAD_REQUEST, ERR_BODY_TOO_LONG_BYTES);
            return;
        }

        String body = ((BodyParser.ParseResult.Success) parseRes).body();

        try {
            Database.InsertPostResult created = db.createPost(user.userId(), body);
            ByteBuf buf = ctx.alloc().buffer(1024);
            FastJsonWriter.writePostResponse(buf, created.id(), body, created.createdAt(), user.username(), 0);
            sendResponse(ctx, req, HttpResponseStatus.CREATED, buf);
        } catch (Exception e) {
            sendStaticError(ctx, req, HttpResponseStatus.INTERNAL_SERVER_ERROR, ERR_INTERNAL_SERVER_ERROR_BYTES);
        }
    }

    private void handleHealth(ChannelHandlerContext ctx, FullHttpRequest req, HttpMethod method) {
        if (method != HttpMethod.GET) {
            sendStaticError(ctx, req, HttpResponseStatus.NOT_FOUND, ERR_NOT_FOUND_BYTES);
            return;
        }
        if (db.checkHealth()) {
            ByteBuf buf = ctx.alloc().buffer(128);
            int uptimeSeconds = (int) ((System.currentTimeMillis() - startTime) / 1000L);
            FastJsonWriter.writeHealth(buf, uptimeSeconds);
            sendResponse(ctx, req, HttpResponseStatus.OK, buf);
        } else {
            ByteBuf buf = Unpooled.wrappedBuffer(ERR_DB_UNHEALTHY_BYTES);
            sendResponse(ctx, req, HttpResponseStatus.SERVICE_UNAVAILABLE, buf);
        }
    }

    private void sendStaticError(ChannelHandlerContext ctx, FullHttpRequest req, HttpResponseStatus status, byte[] errorBytes) {
        ByteBuf buf = Unpooled.wrappedBuffer(errorBytes);
        sendResponse(ctx, req, status, buf);
    }

    private void sendError(ChannelHandlerContext ctx, FullHttpRequest req, HttpResponseStatus status, String message) {
        ByteBuf buf = ctx.alloc().buffer(128);
        FastJsonWriter.writeError(buf, message);
        sendResponse(ctx, req, status, buf);
    }

    private void sendResponse(ChannelHandlerContext ctx, FullHttpRequest req, HttpResponseStatus status, ByteBuf content) {
        boolean keepAlive = HttpUtil.isKeepAlive(req);
        boolean isKeepAliveDefault = req.protocolVersion().isKeepAliveDefault();

        FullHttpResponse response = new DefaultFullHttpResponse(HttpVersion.HTTP_1_1, status, content, false);
        response.headers().set(HttpHeaderNames.CONTENT_TYPE, APPLICATION_JSON);
        response.headers().setInt(HttpHeaderNames.CONTENT_LENGTH, content.readableBytes());

        if (keepAlive) {
            if (!isKeepAliveDefault) {
                response.headers().set(HttpHeaderNames.CONNECTION, HttpHeaderValues.KEEP_ALIVE);
            }
            ctx.write(response);
        } else {
            response.headers().set(HttpHeaderNames.CONNECTION, HttpHeaderValues.CLOSE);
            ctx.write(response).addListener(ChannelFutureListener.CLOSE);
        }
    }
}
