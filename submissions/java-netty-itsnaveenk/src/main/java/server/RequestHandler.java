package server;

import io.netty.buffer.ByteBuf;
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
import server.db.Database;
import server.json.FastJsonWriter;
import server.util.BodyParser;
import server.util.FastJwt;
import server.util.StringUtil;

import java.nio.charset.StandardCharsets;
import java.util.List;

@ChannelHandler.Sharable
public final class RequestHandler extends SimpleChannelInboundHandler<FullHttpRequest> {

    private final Database db;
    private final FastJwt jwt;
    private final long startTime;

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
            if ("/health".equals(uri)) {
                handleHealth(ctx, req, method);
            } else if ("/feed".equals(uri)) {
                handleFeed(ctx, req, method);
            } else if ("/posts".equals(uri)) {
                handleCreatePost(ctx, req, method);
            } else if (uri.startsWith("/posts/")) {
                handlePostsParameterized(ctx, req, method, uri);
            } else {
                sendError(ctx, req, HttpResponseStatus.NOT_FOUND, "not found");
            }
        } catch (Exception e) {
            sendError(ctx, req, HttpResponseStatus.INTERNAL_SERVER_ERROR, "internal server error");
        }
    }

    private void handleHealth(ChannelHandlerContext ctx, FullHttpRequest req, HttpMethod method) {
        if (method != HttpMethod.GET) {
            sendError(ctx, req, HttpResponseStatus.NOT_FOUND, "not found");
            return;
        }
        ByteBuf buf = ctx.alloc().buffer();
        if (db.checkHealth()) {
            int uptimeSeconds = (int) ((System.currentTimeMillis() - startTime) / 1000L);
            FastJsonWriter.writeHealth(buf, uptimeSeconds);
            sendResponse(ctx, req, HttpResponseStatus.OK, buf);
        } else {
            sendError(ctx, req, HttpResponseStatus.INTERNAL_SERVER_ERROR, "database unhealthy");
        }
    }

    private void handleFeed(ChannelHandlerContext ctx, FullHttpRequest req, HttpMethod method) throws Exception {
        if (method != HttpMethod.GET) {
            sendError(ctx, req, HttpResponseStatus.NOT_FOUND, "not found");
            return;
        }

        List<Database.Post> posts = db.getFeed();
        ByteBuf buf = ctx.alloc().buffer();
        FastJsonWriter.writeFeedResponse(buf, posts);
        sendResponse(ctx, req, HttpResponseStatus.OK, buf);
    }

    private void handleCreatePost(ChannelHandlerContext ctx, FullHttpRequest req, HttpMethod method) {
        if (method != HttpMethod.POST) {
            sendError(ctx, req, HttpResponseStatus.NOT_FOUND, "not found");
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
            sendError(ctx, req, HttpResponseStatus.BAD_REQUEST, "malformed JSON body");
            return;
        }
        if (parseRes instanceof BodyParser.ParseResult.BodyRequired) {
            sendError(ctx, req, HttpResponseStatus.BAD_REQUEST, "body is required");
            return;
        }
        if (parseRes instanceof BodyParser.ParseResult.BodyTooLong) {
            sendError(ctx, req, HttpResponseStatus.BAD_REQUEST, "body must be at most 500 characters");
            return;
        }

        String body = ((BodyParser.ParseResult.Success) parseRes).body();
        boolean keepAlive = HttpUtil.isKeepAlive(req);
        boolean isKeepAliveDefault = req.protocolVersion().isKeepAliveDefault();

        db.submitPost(user.userId(), body).whenComplete((created, err) -> {
            ctx.executor().execute(() -> {
                if (!ctx.channel().isActive()) {
                    return;
                }
                if (err != null) {
                    sendErrorDirect(ctx, HttpResponseStatus.INTERNAL_SERVER_ERROR, "internal server error", keepAlive, isKeepAliveDefault);
                    return;
                }
                ByteBuf buf = ctx.alloc().buffer();
                FastJsonWriter.writePostResponse(buf, created.id(), body, created.createdAt(), user.username(), 0);
                sendResponseDirect(ctx, HttpResponseStatus.CREATED, buf, keepAlive, isKeepAliveDefault);
            });
        });
    }

    private void handlePostsParameterized(ChannelHandlerContext ctx, FullHttpRequest req, HttpMethod method, String uri) throws Exception {
        int slashIndex = uri.indexOf('/', 7);
        if (slashIndex >= 0) {
            // There is a slash after /posts/
            if ("/like".equals(uri.substring(slashIndex))) {
                // POST /posts/:id/like
                if (method != HttpMethod.POST) {
                    sendError(ctx, req, HttpResponseStatus.NOT_FOUND, "not found");
                    return;
                }

                // SPEC Check 22: Auth FIRST before ID!
                FastJwt.AuthResult auth = jwt.authenticate(req.headers().get(HttpHeaderNames.AUTHORIZATION));
                if (auth instanceof FastJwt.AuthResult.Failure f) {
                    sendError(ctx, req, HttpResponseStatus.valueOf(f.statusCode()), f.errorMessage());
                    return;
                }
                FastJwt.AuthResult.Success user = (FastJwt.AuthResult.Success) auth;

                String idStr = uri.substring(7, slashIndex);
                Long postId = StringUtil.parsePositiveId(idStr);
                if (postId == null) {
                    sendError(ctx, req, HttpResponseStatus.BAD_REQUEST, "invalid post id");
                    return;
                }

                boolean keepAlive = HttpUtil.isKeepAlive(req);
                boolean isKeepAliveDefault = req.protocolVersion().isKeepAliveDefault();

                db.submitLike(user.userId(), postId).whenComplete((likeRes, err) -> {
                    ctx.executor().execute(() -> {
                        if (!ctx.channel().isActive()) {
                            return;
                        }
                        if (err != null) {
                            sendErrorDirect(ctx, HttpResponseStatus.INTERNAL_SERVER_ERROR, "internal server error", keepAlive, isKeepAliveDefault);
                            return;
                        }
                        if (likeRes == Database.LikeResult.CREATED) {
                            ByteBuf buf = ctx.alloc().buffer();
                            FastJsonWriter.writeLikeResponse(buf, false, postId);
                            sendResponseDirect(ctx, HttpResponseStatus.CREATED, buf, keepAlive, isKeepAliveDefault);
                        } else if (likeRes == Database.LikeResult.ALREADY_LIKED) {
                            ByteBuf buf = ctx.alloc().buffer();
                            FastJsonWriter.writeLikeResponse(buf, true, postId);
                            sendResponseDirect(ctx, HttpResponseStatus.OK, buf, keepAlive, isKeepAliveDefault);
                        } else {
                            sendErrorDirect(ctx, HttpResponseStatus.NOT_FOUND, "post not found", keepAlive, isKeepAliveDefault);
                        }
                    });
                });
            } else {
                // Unknown path after /posts/
                sendError(ctx, req, HttpResponseStatus.NOT_FOUND, "not found");
            }
        } else {
            // GET /posts/:id
            if (method != HttpMethod.GET) {
                sendError(ctx, req, HttpResponseStatus.NOT_FOUND, "not found");
                return;
            }
            String idStr = uri.substring(7);
            Long postId = StringUtil.parsePositiveId(idStr);
            if (postId == null) {
                sendError(ctx, req, HttpResponseStatus.BAD_REQUEST, "invalid post id");
                return;
            }

            Database.Post post = db.getPostById(postId);
            if (post == null) {
                sendError(ctx, req, HttpResponseStatus.NOT_FOUND, "post not found");
                return;
            }

            ByteBuf buf = ctx.alloc().buffer();
            FastJsonWriter.writePostResponse(buf, post.id(), post.body(), post.createdAt(), post.author(), post.likeCount());
            sendResponse(ctx, req, HttpResponseStatus.OK, buf);
        }
    }

    private void sendError(ChannelHandlerContext ctx, FullHttpRequest req, HttpResponseStatus status, String message) {
        sendErrorDirect(ctx, status, message, HttpUtil.isKeepAlive(req), req.protocolVersion().isKeepAliveDefault());
    }

    private void sendResponse(ChannelHandlerContext ctx, FullHttpRequest req, HttpResponseStatus status, ByteBuf content) {
        sendResponseDirect(ctx, status, content, HttpUtil.isKeepAlive(req), req.protocolVersion().isKeepAliveDefault());
    }

    private void sendErrorDirect(ChannelHandlerContext ctx, HttpResponseStatus status, String message, boolean keepAlive, boolean isKeepAliveDefault) {
        ByteBuf buf = ctx.alloc().buffer();
        FastJsonWriter.writeError(buf, message);
        sendResponseDirect(ctx, status, buf, keepAlive, isKeepAliveDefault);
    }

    private void sendResponseDirect(ChannelHandlerContext ctx, HttpResponseStatus status, ByteBuf content, boolean keepAlive, boolean isKeepAliveDefault) {
        FullHttpResponse response = new DefaultFullHttpResponse(HttpVersion.HTTP_1_1, status, content);
        response.headers().set(HttpHeaderNames.CONTENT_TYPE, "application/json");
        response.headers().setInt(HttpHeaderNames.CONTENT_LENGTH, content.readableBytes());

        if (keepAlive) {
            if (!isKeepAliveDefault) {
                response.headers().set(HttpHeaderNames.CONNECTION, HttpHeaderValues.KEEP_ALIVE);
            }
            ctx.writeAndFlush(response);
        } else {
            response.headers().set(HttpHeaderNames.CONNECTION, HttpHeaderValues.CLOSE);
            ctx.writeAndFlush(response).addListener(ChannelFutureListener.CLOSE);
        }
    }
}
