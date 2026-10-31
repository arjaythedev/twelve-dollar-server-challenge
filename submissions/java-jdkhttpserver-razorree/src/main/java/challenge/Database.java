package challenge;

import java.nio.file.*;
import java.sql.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.locks.ReentrantLock;

final class Database implements AutoCloseable {
    private final Sqlite.Conn db;
    private final ReentrantLock lock = new ReentrantLock();
    private final Sqlite.Stmt feed, post, create, like, exists, health;
    private final BlockingQueue<Pending<?>> writes = new LinkedBlockingQueue<>();
    private final Object submissions = new Object();
    private final Pending<Void> stop = new Pending<>(() -> null);
    private final Thread writer;
    private volatile boolean closing;
    private volatile SQLException failure;

    @FunctionalInterface interface Opener {
        Sqlite.Conn open(String path, Properties properties) throws SQLException;
    }
    private static final String SELECT = """
        SELECT p.id, p.body, p.created_at, u.username,
               (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
          FROM posts p JOIN users u ON u.id = p.user_id
        """;

    Database(String path) throws SQLException { this(path, Sqlite::open); }

    Database(String path, Opener opener) throws SQLException {
        if (!Files.isRegularFile(Path.of(path))) throw new SQLException("SQLITE_PATH must point to an existing seed database copy");
        db = opener.open(Path.of(path).toAbsolutePath().toString(), new Properties());
        try {
            try (Sqlite.Stmt s = db.createStatement()) {
                s.execute("PRAGMA journal_mode=WAL");
                s.execute("PRAGMA synchronous=NORMAL");
                s.execute("PRAGMA foreign_keys=ON");
                s.execute("PRAGMA busy_timeout=5000");
                s.execute("PRAGMA cache_size=-65536");
                s.execute("PRAGMA mmap_size=1073741824");
                s.execute("PRAGMA temp_store=MEMORY");
            }
            feed = db.prepareStatement(SELECT + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
            post = db.prepareStatement(SELECT + " WHERE p.id = ?");
            create = db.prepareStatement("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at");
            like = db.prepareStatement("""
                INSERT INTO likes (user_id, post_id)
                SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)
                ON CONFLICT (user_id, post_id) DO NOTHING
                """);
            exists = db.prepareStatement("SELECT 1 FROM posts WHERE id = ?");
            health = db.prepareStatement("SELECT 1");
        } catch (SQLException | RuntimeException error) {
            try { db.close(); } catch (SQLException close) { error.addSuppressed(close); }
            throw error;
        }
        writer = Thread.ofPlatform().name("database-writer").start(this::writeLoop);
    }

    record Post(long id, String body, String createdAt, String author, long likeCount) {}

    private static Post postObject(Sqlite.Rows row) throws SQLException {
        return new Post(row.getLong(1), row.getString(2), row.getString(3), row.getString(4), row.getLong(5));
    }

    private void ensureOpen() throws SQLException {
        if (failure != null) throw new SQLException("Database writer failed", failure);
        if (closing) throw new SQLException("Database is closing");
    }

    List<Post> feed() throws SQLException {
        lock.lock();
        try {
            ensureOpen();
            try (Sqlite.Rows rows = feed.executeQuery()) {
                List<Post> posts = new ArrayList<>(20);
                while (rows.next()) posts.add(postObject(rows));
                return posts;
            }
        } finally { lock.unlock(); }
    }

    byte[] feedJson() throws SQLException {
        lock.lock();
        try {
            ensureOpen();
            try (Sqlite.Rows rows = feed.executeQuery()) { return Json.feedBytes(rows); }
        } finally { lock.unlock(); }
    }

    Post post(long id) throws SQLException {
        lock.lock();
        try {
            ensureOpen();
            post.setLong(1, id);
            try (Sqlite.Rows rows = post.executeQuery()) { return rows.next() ? postObject(rows) : null; }
        } finally { lock.unlock(); }
    }

    byte[] postJson(long id) throws SQLException {
        lock.lock();
        try {
            ensureOpen();
            post.setLong(1, id);
            try (Sqlite.Rows rows = post.executeQuery()) {
                return rows.next() ? Json.postBytes(rows) : null;
            }
        } finally { lock.unlock(); }
    }

    void health() throws SQLException {
        lock.lock();
        try {
            ensureOpen();
            try (Sqlite.Rows rows = health.executeQuery()) {
                if (!rows.next() || rows.getInt(1) != 1) throw new SQLException("SELECT 1 failed");
            }
        } finally { lock.unlock(); }
    }

    private interface Write<T> { T execute() throws SQLException; }

    private static final class Pending<T> {
        final Write<T> action;
        final CompletableFuture<T> completion = new CompletableFuture<>();
        T value;
        SQLException error;

        Pending(Write<T> action) { this.action = action; }
        void execute() throws SQLException { value = action.execute(); }
        void finish() {
            if (error == null) completion.complete(value);
            else completion.completeExceptionally(error);
        }
        void fail(Throwable cause) { completion.completeExceptionally(cause); }
    }

    private <T> T transaction(Write<T> action) throws SQLException {
        Pending<T> pending = new Pending<>(action);
        synchronized (submissions) {
            ensureOpen();
            writes.add(pending);
        }
        try {
            return pending.completion.get();
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new SQLException("Interrupted while waiting for database write", error);
        } catch (ExecutionException error) {
            if (error.getCause() instanceof SQLException sql) throw sql;
            throw new SQLException("Database write failed", error.getCause());
        }
    }

    private static SQLException sqlFailure(Throwable error) {
        return error instanceof SQLException sql ? sql : new SQLException("Database writer failed", error);
    }

    private void writeBatch(List<Pending<?>> batch) throws SQLException {
        lock.lock();
        try {
            try {
                db.setAutoCommit(false); // Configured as BEGIN IMMEDIATE.
                for (Pending<?> pending : batch) {
                    String point = db.setSavepoint("request");
                    try {
                        pending.execute(); // RETURNING result sets close before this returns.
                    } catch (SQLException error) {
                        // Constraint failures are request-local. I/O/state errors fail the batch.
                        if ((error.getErrorCode() & 255) != 19 || error.getSuppressed().length != 0) throw error;
                        db.rollback(point);
                        pending.error = error;
                    }
                    db.releaseSavepoint(point);
                }
                db.commit();
            } catch (Throwable error) {
                SQLException fatal = sqlFailure(error);
                try { if (!db.getAutoCommit()) db.rollback(); }
                catch (Throwable rollback) { fatal.addSuppressed(rollback); }
                // Never turn auto-commit on after an uncertain rollback: it could commit failed work.
                failure = fatal;
                try { db.close(); } catch (Throwable close) { fatal.addSuppressed(close); }
                for (Pending<?> pending : batch) pending.fail(fatal);
                throw fatal;
            }
        } finally { lock.unlock(); }
        // No HTTP handler can observe success until COMMIT has succeeded.
        for (Pending<?> pending : batch) pending.finish();
    }

    private void writeLoop() {
        try {
            while (true) {
                Pending<?> first = writes.take();
                if (first == stop) break;
                List<Pending<?>> batch = new ArrayList<>(256);
                batch.add(first);
                writes.drainTo(batch, 255); // No batching delay; process available work immediately.
                boolean stopped = batch.remove(stop);
                writeBatch(batch);
                if (stopped) break;
            }
        } catch (Throwable error) {
            failure = sqlFailure(error);
            System.err.println("Database writer failed: " + error);
            error.printStackTrace(System.err);
        } finally {
            SQLException error = failure == null ? new SQLException("Database is closing") : failure;
            // Exclude concurrent submissions before draining a failed worker's queue.
            synchronized (submissions) {
                if (failure != null) closing = true;
                Pending<?> pending;
                while ((pending = writes.poll()) != null) if (pending != stop) pending.fail(error);
            }
        }
    }

    Post create(Auth.User user, String body) throws SQLException {
        return transaction(() -> {
            create.setLong(1, user.id());
            create.setString(2, body);
            try (Sqlite.Rows row = create.executeQuery()) {
                if (!row.next()) throw new SQLException("Insert returned no row");
                var result = new Post(row.getLong(1), body, row.getString(2), user.username(), 0);
                if (row.next()) throw new SQLException("Insert returned multiple rows");
                return result;
            }
        });
    }

    // 1 = inserted, 0 = already liked, -1 = missing post.
    int like(long userId, long postId) throws SQLException {
        return transaction(() -> {
            like.setLong(1, userId);
            like.setLong(2, postId);
            if (like.executeUpdate() == 1) return 1;
            exists.setLong(1, postId);
            try (Sqlite.Rows row = exists.executeQuery()) { return row.next() ? 0 : -1; }
        });
    }

    @Override public void close() throws SQLException {
        List<Pending<?>> abandoned = new ArrayList<>();
        synchronized (submissions) {
            if (!closing) {
                closing = true;
                writes.drainTo(abandoned);
                writes.add(stop);
            }
        }
        SQLException stopped = new SQLException("Database is closing");
        for (Pending<?> pending : abandoned) if (pending != stop) pending.fail(stopped);
        boolean interrupted = false;
        while (writer.isAlive()) {
            try { writer.join(); }
            catch (InterruptedException error) { interrupted = true; }
        }
        lock.lock();
        try { db.close(); } finally {
            lock.unlock();
            if (interrupted) Thread.currentThread().interrupt();
        }
    }
}
