package server.db;

import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

public final class Database implements AutoCloseable {

    public record Post(long id, String body, String createdAt, String author, int likeCount) {}
    public record InsertPostResult(long id, String createdAt) {}
    public enum LikeResult {
        CREATED,
        ALREADY_LIKED,
        NOT_FOUND
    }

    public sealed interface WriteTask permits PostTask, LikeTask {}

    public static final class PostTask implements WriteTask {
        public final long userId;
        public final String body;
        public final CompletableFuture<InsertPostResult> future = new CompletableFuture<>();
        public InsertPostResult result;
        public Throwable error;

        public PostTask(long userId, String body) {
            this.userId = userId;
            this.body = body;
        }
    }

    public static final class LikeTask implements WriteTask {
        public final long userId;
        public final long postId;
        public final CompletableFuture<LikeResult> future = new CompletableFuture<>();
        public LikeResult result;
        public Throwable error;

        public LikeTask(long userId, long postId) {
            this.userId = userId;
            this.postId = postId;
        }
    }

    private final Connection connRead;
    private final Connection connWrite;

    private final PreparedStatement healthStmt;
    private final PreparedStatement feedStmt;
    private final PreparedStatement postByIdStmt;

    private final PreparedStatement insertPostStmt;
    private final PreparedStatement insertLikeStmt;
    private final PreparedStatement postExistsStmt;

    private final BlockingQueue<WriteTask> writeQueue = new LinkedBlockingQueue<>();
    private final Thread writerThread;
    private volatile boolean running = true;

    private static final String POST_SELECT = """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
              FROM posts p JOIN users u ON u.id = p.user_id
            """;

    private static final String FEED_SQL = POST_SELECT + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
    private static final String POST_BY_ID_SQL = POST_SELECT + " WHERE p.id = ?";
    private static final String INSERT_POST_SQL = "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at";
    private static final String INSERT_LIKE_SQL = """
            INSERT INTO likes (user_id, post_id)
            SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)
            ON CONFLICT (user_id, post_id) DO NOTHING
            """;
    private static final String POST_EXISTS_SQL = "SELECT 1 FROM posts WHERE id = ?";

    public Database(String sqlitePath) throws SQLException {
        // Read connection: optimized for concurrent non-blocking reads in WAL mode
        this.connRead = DriverManager.getConnection("jdbc:sqlite:" + sqlitePath);
        this.connRead.setAutoCommit(true);
        try (Statement s = connRead.createStatement()) {
            s.execute("PRAGMA journal_mode = WAL;");
            s.execute("PRAGMA synchronous = NORMAL;");
            s.execute("PRAGMA mmap_size = 1073741824;");
            s.execute("PRAGMA cache_size = -65536;");
            s.execute("PRAGMA temp_store = MEMORY;");
            s.execute("PRAGMA busy_timeout = 5000;");
        }

        // Write connection: dedicated connection for group commit write batches
        this.connWrite = DriverManager.getConnection("jdbc:sqlite:" + sqlitePath);
        this.connWrite.setAutoCommit(true);
        try (Statement s = connWrite.createStatement()) {
            s.execute("PRAGMA synchronous = NORMAL;");
            s.execute("PRAGMA mmap_size = 1073741824;");
            s.execute("PRAGMA cache_size = -65536;");
            s.execute("PRAGMA temp_store = MEMORY;");
            s.execute("PRAGMA busy_timeout = 5000;");
        }

        this.healthStmt = connRead.prepareStatement("SELECT 1");
        this.feedStmt = connRead.prepareStatement(FEED_SQL);
        this.postByIdStmt = connRead.prepareStatement(POST_BY_ID_SQL);

        this.insertPostStmt = connWrite.prepareStatement(INSERT_POST_SQL);
        this.insertLikeStmt = connWrite.prepareStatement(INSERT_LIKE_SQL);
        this.postExistsStmt = connWrite.prepareStatement(POST_EXISTS_SQL);

        this.writerThread = new Thread(this::runWriterLoop, "sqlite-writer");
        this.writerThread.setDaemon(true);
        this.writerThread.start();
    }

    private void runWriterLoop() {
        List<WriteTask> batch = new ArrayList<>(64);
        while (running) {
            try {
                WriteTask first = writeQueue.poll(50, TimeUnit.MILLISECONDS);
                if (first == null) {
                    continue;
                }
                batch.add(first);
                while (batch.size() < 64) {
                    WriteTask next = writeQueue.poll();
                    if (next == null) break;
                    batch.add(next);
                }

                executeBatch(batch);
                batch.clear();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            } catch (Throwable ignored) {}
        }

        // Drain any remaining tasks on shutdown
        WriteTask remaining;
        while ((remaining = writeQueue.poll()) != null) {
            batch.add(remaining);
        }
        if (!batch.isEmpty()) {
            executeBatch(batch);
        }
    }

    private void executeBatch(List<WriteTask> batch) {
        if (batch.isEmpty()) return;
        try {
            connWrite.setAutoCommit(false);
            for (WriteTask task : batch) {
                if (task instanceof PostTask pt) {
                    insertPostStmt.setLong(1, pt.userId);
                    insertPostStmt.setString(2, pt.body);
                    try (ResultSet rs = insertPostStmt.executeQuery()) {
                        if (rs.next()) {
                            pt.result = new InsertPostResult(rs.getLong(1), rs.getString(2));
                        } else {
                            pt.error = new SQLException("Failed to retrieve created post from RETURNING clause");
                        }
                    }
                } else if (task instanceof LikeTask lt) {
                    insertLikeStmt.setLong(1, lt.userId);
                    insertLikeStmt.setLong(2, lt.postId);
                    int rows = insertLikeStmt.executeUpdate();
                    if (rows == 1) {
                        lt.result = LikeResult.CREATED;
                    } else {
                        postExistsStmt.setLong(1, lt.postId);
                        try (ResultSet rs = postExistsStmt.executeQuery()) {
                            if (rs.next()) {
                                lt.result = LikeResult.ALREADY_LIKED;
                            } else {
                                lt.result = LikeResult.NOT_FOUND;
                            }
                        }
                    }
                }
            }
            connWrite.commit();
            connWrite.setAutoCommit(true);

            for (WriteTask task : batch) {
                if (task instanceof PostTask pt) {
                    if (pt.error != null) pt.future.completeExceptionally(pt.error);
                    else pt.future.complete(pt.result);
                } else if (task instanceof LikeTask lt) {
                    if (lt.error != null) lt.future.completeExceptionally(lt.error);
                    else lt.future.complete(lt.result);
                }
            }
        } catch (Throwable t) {
            try {
                connWrite.rollback();
                connWrite.setAutoCommit(true);
            } catch (Exception ignored) {}

            for (WriteTask task : batch) {
                executeFallback(task);
            }
        }
    }

    private void executeFallback(WriteTask task) {
        try {
            if (task instanceof PostTask pt) {
                insertPostStmt.setLong(1, pt.userId);
                insertPostStmt.setString(2, pt.body);
                try (ResultSet rs = insertPostStmt.executeQuery()) {
                    if (rs.next()) {
                        pt.future.complete(new InsertPostResult(rs.getLong(1), rs.getString(2)));
                        return;
                    }
                }
                pt.future.completeExceptionally(new SQLException("Insert failed"));
            } else if (task instanceof LikeTask lt) {
                insertLikeStmt.setLong(1, lt.userId);
                insertLikeStmt.setLong(2, lt.postId);
                int rows = insertLikeStmt.executeUpdate();
                if (rows == 1) {
                    lt.future.complete(LikeResult.CREATED);
                } else {
                    postExistsStmt.setLong(1, lt.postId);
                    try (ResultSet rs = postExistsStmt.executeQuery()) {
                        if (rs.next()) {
                            lt.future.complete(LikeResult.ALREADY_LIKED);
                        } else {
                            lt.future.complete(LikeResult.NOT_FOUND);
                        }
                    }
                }
            }
        } catch (Throwable err) {
            if (task instanceof PostTask pt) pt.future.completeExceptionally(err);
            else if (task instanceof LikeTask lt) lt.future.completeExceptionally(err);
        }
    }

    public boolean checkHealth() {
        try (ResultSet rs = healthStmt.executeQuery()) {
            return rs.next();
        } catch (SQLException e) {
            return false;
        }
    }

    public List<Post> getFeed() throws SQLException {
        List<Post> posts = new ArrayList<>(20);
        try (ResultSet rs = feedStmt.executeQuery()) {
            while (rs.next()) {
                posts.add(new Post(
                        rs.getLong(1),
                        rs.getString(2),
                        rs.getString(3),
                        rs.getString(4),
                        rs.getInt(5)
                ));
            }
        }
        return posts;
    }

    public Post getPostById(long id) throws SQLException {
        postByIdStmt.setLong(1, id);
        try (ResultSet rs = postByIdStmt.executeQuery()) {
            if (rs.next()) {
                return new Post(
                        rs.getLong(1),
                        rs.getString(2),
                        rs.getString(3),
                        rs.getString(4),
                        rs.getInt(5)
                );
            }
        }
        return null;
    }

    public CompletableFuture<InsertPostResult> submitPost(long userId, String body) {
        PostTask task = new PostTask(userId, body);
        writeQueue.add(task);
        return task.future;
    }

    public CompletableFuture<LikeResult> submitLike(long userId, long postId) {
        LikeTask task = new LikeTask(userId, postId);
        writeQueue.add(task);
        return task.future;
    }

    public InsertPostResult createPost(long userId, String body) throws Exception {
        return submitPost(userId, body).get(5, TimeUnit.SECONDS);
    }

    public LikeResult likePost(long userId, long postId) throws Exception {
        return submitLike(userId, postId).get(5, TimeUnit.SECONDS);
    }

    @Override
    public void close() {
        running = false;
        writerThread.interrupt();
        try {
            writerThread.join(2000);
        } catch (InterruptedException ignored) {}

        closeQuietly(healthStmt);
        closeQuietly(feedStmt);
        closeQuietly(postByIdStmt);
        closeQuietly(insertPostStmt);
        closeQuietly(insertLikeStmt);
        closeQuietly(postExistsStmt);
        closeQuietly(connRead);
        closeQuietly(connWrite);
    }

    private static void closeQuietly(AutoCloseable closeable) {
        if (closeable != null) {
            try {
                closeable.close();
            } catch (Exception ignored) {}
        }
    }
}
