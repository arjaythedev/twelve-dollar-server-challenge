package server;

import java.sql.*;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.*;

public final class Database {
    private static final int READER_POOL_SIZE = 8;
    private static final ArrayBlockingQueue<ReaderConnection> READER_POOL = new ArrayBlockingQueue<>(READER_POOL_SIZE);
    private static final BlockingQueue<WriteRequest> WRITE_QUEUE = new LinkedBlockingQueue<>();
    private static volatile boolean running = true;
    private static String dbUrl;

    public static class ReaderConnection {
        public final Connection conn;
        public final PreparedStatement feedStmt;
        public final PreparedStatement postByIdStmt;
        public final PreparedStatement healthStmt;

        public ReaderConnection(Connection conn) throws SQLException {
            this.conn = conn;
            this.feedStmt = conn.prepareStatement(
                "SELECT json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username, 'like_count', " +
                "(SELECT COUNT(*) FROM likes l WHERE l.post_id = p.id)) " +
                "FROM posts p " +
                "JOIN users u ON u.id = p.user_id " +
                "ORDER BY p.created_at DESC, p.id DESC " +
                "LIMIT 20"
            );
            this.postByIdStmt = conn.prepareStatement(
                "SELECT json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username, 'like_count', " +
                "(SELECT COUNT(*) FROM likes l WHERE l.post_id = p.id)) " +
                "FROM posts p " +
                "JOIN users u ON u.id = p.user_id " +
                "WHERE p.id = ?"
            );
            this.healthStmt = conn.prepareStatement("SELECT 1");
        }
    }

    public static abstract class WriteRequest {
        public final CompletableFuture<DbResult> future = new CompletableFuture<>();
        public DbResult result;
        public abstract void execute(Connection conn) throws SQLException;
    }

    public static class DbResult {
        public int status = 200;
        public String body;
    }

    public static class CreatePostRequest extends WriteRequest {
        private final long userId;
        private final String body;
        private final String username;

        public CreatePostRequest(long userId, String body, String username) {
            this.userId = userId;
            this.body = body;
            this.username = username;
        }

        @Override
        public void execute(Connection conn) throws SQLException {
            try (PreparedStatement stmt = conn.prepareStatement(
                "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at"
            )) {
                stmt.setLong(1, userId);
                stmt.setString(2, body);
                try (ResultSet rs = stmt.executeQuery()) {
                    if (rs.next()) {
                        long newId = rs.getLong(1);
                        String createdAt = rs.getString(2);
                        result = new DbResult();
                        result.status = 201;
                        result.body = "{\"post\":{\"id\":" + newId +
                                      ",\"body\":" + JsonUtil.escapeJson(body) +
                                      ",\"created_at\":\"" + createdAt +
                                      "\",\"author\":\"" + username +
                                      "\",\"like_count\":0}}";
                    } else {
                        throw new SQLException("Failed to retrieve generated post id");
                    }
                }
            }
        }
    }

    public static class LikePostRequest extends WriteRequest {
        private final long userId;
        private final long postId;

        public LikePostRequest(long userId, long postId) {
            this.userId = userId;
            this.postId = postId;
        }

        @Override
        public void execute(Connection conn) throws SQLException {
            try (PreparedStatement stmt = conn.prepareStatement(
                "INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT DO NOTHING"
            )) {
                stmt.setLong(1, userId);
                stmt.setLong(2, postId);
                int rows = stmt.executeUpdate();
                result = new DbResult();
                if (rows == 1) {
                    result.status = 201;
                    result.body = "{\"liked\":true,\"already_liked\":false,\"post_id\":" + postId + "}";
                } else {
                    result.status = 200;
                    result.body = "{\"liked\":true,\"already_liked\":true,\"post_id\":" + postId + "}";
                }
            }
        }
    }

    public static void init(String sqlitePath) throws Exception {
        Class.forName("org.sqlite.JDBC");
        dbUrl = "jdbc:sqlite:" + sqlitePath;

        // Initialize readers
        for (int i = 0; i < READER_POOL_SIZE; i++) {
            Connection conn = DriverManager.getConnection(dbUrl);
            configureConnection(conn, true);
            READER_POOL.put(new ReaderConnection(conn));
        }

        // Start dedicated Writer thread with Group Commit
        Thread.ofPlatform().daemon(true).name("sqlite-group-committer").start(Database::runWriterLoop);
    }

    private static void configureConnection(Connection conn, boolean readOnly) throws SQLException {
        try (Statement stmt = conn.createStatement()) {
            stmt.execute("PRAGMA journal_mode = WAL");
            stmt.execute("PRAGMA synchronous = NORMAL");
            stmt.execute("PRAGMA foreign_keys = ON");
            stmt.execute("PRAGMA temp_store = MEMORY");
            stmt.execute("PRAGMA cache_size = -65536"); // 64 MB
            stmt.execute("PRAGMA mmap_size = 1073741824"); // 1 GB
            stmt.execute("PRAGMA busy_timeout = 5000");
            if (readOnly) {
                stmt.execute("PRAGMA query_only = ON");
            }
        }
    }

    private static void runWriterLoop() {
        Connection writeConn = null;
        try {
            writeConn = DriverManager.getConnection(dbUrl);
            configureConnection(writeConn, false);

            List<WriteRequest> batch = new ArrayList<>(128);

            while (running) {
                WriteRequest first = WRITE_QUEUE.take();
                batch.clear();
                batch.add(first);
                WRITE_QUEUE.drainTo(batch, 127);

                try {
                    writeConn.setAutoCommit(false);
                    for (WriteRequest req : batch) {
                        req.execute(writeConn);
                    }
                    writeConn.commit();
                    writeConn.setAutoCommit(true);
                    for (WriteRequest req : batch) {
                        req.future.complete(req.result);
                    }
                } catch (Exception e) {
                    try { writeConn.rollback(); } catch (Exception ignored) {}
                    // If batch failed (e.g. FK constraint error on missing post), retry individually
                    for (WriteRequest req : batch) {
                        try {
                            writeConn.setAutoCommit(true);
                            req.execute(writeConn);
                            req.future.complete(req.result);
                        } catch (Exception ex) {
                            req.future.completeExceptionally(ex);
                        }
                    }
                }
            }
        } catch (Exception e) {
            e.printStackTrace();
        } finally {
            if (writeConn != null) {
                try { writeConn.close(); } catch (Exception ignored) {}
            }
        }
    }

    public static DbResult checkHealth(long startTimeMs) {
        ReaderConnection r = null;
        try {
            r = READER_POOL.take();
            r.healthStmt.execute();
            long uptimeSeconds = (System.currentTimeMillis() - startTimeMs) / 1000;
            DbResult res = new DbResult();
            res.status = 200;
            res.body = "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":" + uptimeSeconds + "}";
            return res;
        } catch (Exception e) {
            DbResult res = new DbResult();
            res.status = 503;
            res.body = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":" + JsonUtil.escapeJson(e.getMessage()) + "}";
            return res;
        } finally {
            if (r != null) READER_POOL.offer(r);
        }
    }

    public static DbResult getFeed() {
        ReaderConnection r = null;
        try {
            r = READER_POOL.take();
            StringBuilder sb = new StringBuilder(4096);
            sb.append("{\"posts\":[");
            try (ResultSet rs = r.feedStmt.executeQuery()) {
                boolean first = true;
                while (rs.next()) {
                    if (!first) sb.append(',');
                    sb.append(rs.getString(1));
                    first = false;
                }
            }
            sb.append("]}");
            DbResult res = new DbResult();
            res.status = 200;
            res.body = sb.toString();
            return res;
        } catch (Exception e) {
            DbResult res = new DbResult();
            res.status = 500;
            res.body = "{\"error\":" + JsonUtil.escapeJson(e.getMessage()) + "}";
            return res;
        } finally {
            if (r != null) READER_POOL.offer(r);
        }
    }

    public static DbResult getPostById(long postId) {
        ReaderConnection r = null;
        try {
            r = READER_POOL.take();
            r.postByIdStmt.setLong(1, postId);
            try (ResultSet rs = r.postByIdStmt.executeQuery()) {
                if (rs.next()) {
                    DbResult res = new DbResult();
                    res.status = 200;
                    res.body = "{\"post\":" + rs.getString(1) + "}";
                    return res;
                } else {
                    DbResult res = new DbResult();
                    res.status = 404;
                    res.body = "{\"error\":\"post not found\"}";
                    return res;
                }
            }
        } catch (Exception e) {
            DbResult res = new DbResult();
            res.status = 500;
            res.body = "{\"error\":" + JsonUtil.escapeJson(e.getMessage()) + "}";
            return res;
        } finally {
            if (r != null) READER_POOL.offer(r);
        }
    }

    public static DbResult createPost(long userId, String body, String username) throws Exception {
        CreatePostRequest req = new CreatePostRequest(userId, body, username);
        WRITE_QUEUE.put(req);
        return req.future.get();
    }

    public static DbResult likePost(long userId, long postId) throws Exception {
        LikePostRequest req = new LikePostRequest(userId, postId);
        WRITE_QUEUE.put(req);
        try {
            return req.future.get();
        } catch (ExecutionException e) {
            Throwable cause = e.getCause();
            if (cause instanceof SQLException sqlEx) {
                String msg = sqlEx.getMessage();
                if (msg != null && (msg.contains("FOREIGN KEY") || msg.contains("SQLITE_CONSTRAINT_FOREIGNKEY"))) {
                    DbResult res = new DbResult();
                    res.status = 404;
                    res.body = "{\"error\":\"post not found\"}";
                    return res;
                }
            }
            throw e;
        }
    }
}

