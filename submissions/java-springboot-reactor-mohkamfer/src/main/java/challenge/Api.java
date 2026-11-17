package challenge;

import io.netty.buffer.Unpooled;
import io.netty.handler.codec.http.HttpHeaderNames;
import io.netty.handler.codec.http.HttpMethod;
import io.netty.handler.codec.http.HttpResponseStatus;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import org.reactivestreams.Publisher;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import reactor.core.publisher.Mono;
import reactor.netty.http.server.HttpServerRequest;
import reactor.netty.http.server.HttpServerResponse;

/**
 * The whole API as one Reactor Netty handler, registered on Spring Boot's Netty server through a
 * {@code NettyRouteProvider}. It dispatches the five endpoints by hand and turns every outcome into
 * exact JSON bytes. Reads (inline mode) and every error are answered synchronously on the event loop;
 * only request bodies and writes go through Reactor operators.
 */
final class Api {

    private static final Logger log = LoggerFactory.getLogger(Api.class);
    private static final int MAX_BODY_CHARS = 500;
    private static final int MAX_REQUEST_BYTES = 64 * 1024;
    private static final String CONTENT_TYPE_JSON = "application/json";

    private final Db db;
    private final Jwt jwt;
    private final long startNanos = System.nanoTime();

    Api(Db db, Jwt jwt) {
        this.db = db;
        this.jwt = jwt;
    }

    Publisher<Void> handle(HttpServerRequest req, HttpServerResponse res) {
        Object result;
        try {
            result = route(req);
        } catch (Throwable e) {
            result = toErrorResp(e);
        }
        if (result instanceof Resp r) return send(res, r);
        @SuppressWarnings("unchecked")
        Mono<Resp> async = (Mono<Resp>) result;
        return async.onErrorResume(e -> Mono.just(toErrorResp(e))).flatMap(r -> Mono.from(send(res, r)));
    }

    /** Returns a {@link Resp} when the answer is ready now, or a {@code Mono<Resp>} when it isn't. */
    private Object route(HttpServerRequest req) throws Exception {
        String path = req.uri();
        int q = path.indexOf('?');
        if (q >= 0) path = path.substring(0, q);
        HttpMethod method = req.method();
        if (method == HttpMethod.GET) {
            if (path.equals("/feed")) return read(Db.Reader::feed);
            if (path.equals("/health")) return health();
            if (path.startsWith("/posts/")) {
                String rest = path.substring(7);
                if (rest.indexOf('/') < 0) {
                    long id = Ids.parsePositiveLong(rest);
                    if (id <= 0) return Resp.INVALID_POST_ID;
                    return read(r -> r.post(id));
                }
            }
        } else if (method == HttpMethod.POST) {
            if (path.equals("/posts")) return createPost(req);
            if (path.length() > 12 && path.startsWith("/posts/") && path.endsWith("/like")) {
                String rawId = path.substring(7, path.length() - 5);
                if (rawId.indexOf('/') < 0) return like(req, rawId);
            }
        }
        return Resp.NOT_FOUND;
    }

    private Object read(Db.ReadOp<Resp> op) throws Exception {
        return db.inlineReads() ? db.readNow(op) : db.read(op);
    }

    private Object health() {
        long uptime = (System.nanoTime() - startNanos) / 1_000_000_000L;
        try {
            Db.ReadOp<Resp> ping = r -> {
                r.ping();
                return new Resp(200, ("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":" + uptime + "}")
                        .getBytes(StandardCharsets.US_ASCII));
            };
            if (db.inlineReads()) return db.readNow(ping);
            return db.read(ping).onErrorResume(e -> Mono.just(degradedHealth(e)));
        } catch (Exception e) {
            return degradedHealth(e);
        }
    }

    private static Resp degradedHealth(Throwable e) {
        return new Out().ascii("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":")
                .str(String.valueOf(e.getMessage())).ch('}').resp(503);
    }

    private Mono<Resp> createPost(HttpServerRequest req) {
        Jwt.Auth auth = jwt.verify(req.requestHeaders().get(HttpHeaderNames.AUTHORIZATION));
        return req.receive().aggregate().asString(StandardCharsets.UTF_8)
                .defaultIfEmpty("")
                .flatMap(text -> {
                    if (text.length() > MAX_REQUEST_BYTES) return Mono.just(Resp.BODY_TOO_LONG);
                    Object root;
                    try {
                        root = Json.parse(text);
                    } catch (Json.Malformed e) {
                        return Mono.just(Resp.MALFORMED_JSON);
                    }
                    if (!(root instanceof Map<?, ?> m) || !(m.get("body") instanceof String raw)) {
                        return Mono.just(Resp.BODY_REQUIRED);
                    }
                    String body = raw.strip();
                    if (body.isEmpty()) return Mono.just(Resp.BODY_REQUIRED);
                    if (body.length() > MAX_BODY_CHARS && body.codePointCount(0, body.length()) > MAX_BODY_CHARS) {
                        return Mono.just(Resp.BODY_TOO_LONG);
                    }
                    return db.createPost(auth.userId(), auth.username(), body);
                });
    }

    private Object like(HttpServerRequest req, String rawId) {
        Jwt.Auth auth = jwt.verify(req.requestHeaders().get(HttpHeaderNames.AUTHORIZATION)); // auth before id
        long id = Ids.parsePositiveLong(rawId);
        if (id <= 0) return Resp.INVALID_POST_ID;
        return db.like(auth.userId(), id);
    }

    private static Resp toErrorResp(Throwable e) {
        if (e instanceof ApiError a) return a.resp;
        log.error("request failed", e);
        return Resp.INTERNAL_ERROR;
    }

    private static Publisher<Void> send(HttpServerResponse res, Resp r) {
        res.status(HttpResponseStatus.valueOf(r.status()));
        res.responseHeaders()
                .set(HttpHeaderNames.CONTENT_TYPE, CONTENT_TYPE_JSON)
                .setInt(HttpHeaderNames.CONTENT_LENGTH, r.body().length);
        return res.sendObject(Unpooled.wrappedBuffer(r.body()));
    }
}
