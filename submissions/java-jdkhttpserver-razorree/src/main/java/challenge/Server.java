package challenge;

import com.sun.net.httpserver.*;
import java.io.*;
import java.net.InetSocketAddress;
import java.sql.SQLException;
import java.util.Map;
import java.util.concurrent.*;

public final class Server {
    private static final long START = System.nanoTime();
    private static final int MAX_BODY = 16 * 1024;
    // ECMAScript trim whitespace, matching the reference implementation (not String.trim/strip).
    private static final String WHITESPACE = "\t\n\u000b\f\r \u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000\ufeff";
    private final Database db;
    private final Auth auth;
    record Response(int status, Object body) {}
    static final class ApiError extends Exception {
        final int status;
        ApiError(int status, String message) { super(message); this.status = status; }
    }

    private Server(Database db, Auth auth) { this.db = db; this.auth = auth; }

    public static void main(String[] args) throws Exception {
        // HttpServer reads these properties during initialization.
        System.setProperty("sun.net.httpserver.idleInterval", "75");
        System.setProperty("sun.net.httpserver.maxIdleConnections", "8192");
        System.setProperty("jdk.httpserver.maxConnections", "-1");
        System.setProperty("sun.net.httpserver.nodelay", "true");
        Database database = new Database(required("SQLITE_PATH"));
        Server api = new Server(database, new Auth(required("JWT_SECRET")));
        String host = System.getenv().getOrDefault("HOST", "127.0.0.1");
        int port = Integer.parseInt(System.getenv().getOrDefault("PORT", "3000"));
        HttpServer server = HttpServer.create(new InetSocketAddress(host, port), 4096);
        ExecutorService executor = Executors.newVirtualThreadPerTaskExecutor();
        Semaphore inFlight = new Semaphore(1024);
        server.setExecutor(task -> {
            inFlight.acquireUninterruptibly();
            try {
                executor.execute(() -> {
                    try { task.run(); }
                    finally { inFlight.release(); }
                });
            } catch (RuntimeException error) {
                inFlight.release();
                throw error;
            }
        });
        server.createContext("/", api::handle);
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            server.stop(1);
            executor.shutdownNow();
            try { database.close(); } catch (SQLException e) { System.err.println("Database close failed: " + e); }
        }));
        server.start();
        System.err.println("Java " + Runtime.version() + " HttpServer listening on " + host + ":" + port);
    }

    private static String required(String name) {
        String value = System.getenv(name);
        if (value == null || value.isEmpty()) throw new IllegalArgumentException(name + " is required");
        return value;
    }

    private void handle(HttpExchange exchange) throws IOException {
        try (exchange) {
            Response response;
            try {
                Headers headers = exchange.getRequestHeaders();
                String length = headers.getFirst("Content-Length");
                if ((length == null || length.equals("0")) && headers.getFirst("Transfer-Encoding") == null) {
                    // Observe EOF without a drain buffer when HTTP framing says there is no body.
                    exchange.getRequestBody().read();
                }
                response = route(exchange);
            } catch (ApiError e) {
                response = new Response(e.status, Json.object("error", e.getMessage()));
            } catch (Exception e) {
                System.err.println("Request failed: " + e);
                response = new Response(500, Json.object("error", "internal server error"));
            }
            // SQLite read routes return complete JSON bytes; other routes serialize here.
            byte[] bytes = response.body() instanceof byte[] raw ? raw : Json.encode(response.body());
            exchange.getResponseHeaders().set("Content-Type", "application/json");
            exchange.sendResponseHeaders(response.status(), bytes.length);
            exchange.getResponseBody().write(bytes);
        }
    }

    private Response route(HttpExchange exchange) throws Exception {
        String path = exchange.getRequestURI().getPath();
        String method = exchange.getRequestMethod();
        if (method.equals("GET") && path.equals("/health")) {
            try {
                db.health();
                return new Response(200, Json.object("status", "ok", "db", "ok", "uptime_s", (System.nanoTime() - START) / 1_000_000_000));
            } catch (SQLException e) {
                return new Response(503, Json.object("status", "degraded", "db", "unreachable", "error", e.getMessage()));
            }
        }
        if (method.equals("GET") && path.equals("/feed")) return new Response(200, db.feedJson());
        if (method.equals("POST") && path.equals("/posts")) {
            Auth.User user = auth.verify(exchange.getRequestHeaders().getFirst("Authorization"));
            byte[] bytes = exchange.getRequestBody().readNBytes(MAX_BODY + 1);
            if (bytes.length > MAX_BODY) throw new ApiError(413, "request body too large");
            Object parsed;
            try { parsed = Json.parse(bytes); }
            catch (IOException e) { throw new ApiError(400, "malformed JSON body"); }
            if (!(parsed instanceof Map<?, ?> data) || !(data.get("body") instanceof String body))
                throw new ApiError(400, "body is required");
            body = trim(body);
            if (body.isEmpty()) throw new ApiError(400, "body is required");
            if (body.codePointCount(0, body.length()) > 500) throw new ApiError(400, "body must be at most 500 characters");
            return new Response(201, Json.object("post", db.create(user, body)));
        }
        if (path.startsWith("/posts/")) {
            String remainder = path.substring(7);
            if (method.equals("POST") && remainder.endsWith("/like")) {
                String rawId = remainder.substring(0, remainder.length() - 5);
                if (rawId.contains("/")) throw new ApiError(404, "not found");
                Auth.User user = auth.verify(exchange.getRequestHeaders().getFirst("Authorization"));
                long id = parseId(rawId);
                int changed = id == -1 ? -1 : db.like(user.id(), id);
                if (changed < 0) throw new ApiError(404, "post not found");
                return new Response(changed == 1 ? 201 : 200,
                    Json.object("liked", true, "already_liked", changed == 0, "post_id", id));
            }
            if (method.equals("GET") && !remainder.contains("/")) {
                long id = parseId(remainder);
                var post = id == -1 ? null : db.postJson(id);
                if (post == null) throw new ApiError(404, "post not found");
                return new Response(200, post);
            }
        }
        throw new ApiError(404, "not found");
    }

    static boolean decimalId(String raw) {
        if (raw.isEmpty()) return false;
        for (int i = 0; i < raw.length(); i++) if (raw.charAt(i) < '0' || raw.charAt(i) > '9') return false;
        return true;
    }

    private static long parseId(String raw) throws ApiError {
        if (!decimalId(raw) || raw.chars().allMatch(c -> c == '0')) throw new ApiError(400, "invalid post id");
        try { return Long.parseLong(raw); }
        catch (NumberFormatException e) { return -1; } // Positive IDs outside SQLite's range cannot exist.
    }

    private static String trim(String body) {
        int start = 0, end = body.length();
        while (start < end && WHITESPACE.indexOf(body.charAt(start)) >= 0) start++;
        while (end > start && WHITESPACE.indexOf(body.charAt(end - 1)) >= 0) end--;
        return body.substring(start, end);
    }
}
