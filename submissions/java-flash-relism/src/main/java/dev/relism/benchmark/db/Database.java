package dev.relism.benchmark.db;

import dev.relism.fpr.core.ByteView;
import dev.relism.benchmark.http.Json;
import dev.relism.benchmark.auth.Tokens;
import dev.relism.flash.http.HttpException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import org.slf4j.LoggerFactory;

public final class Database implements AutoCloseable {
    public record Reply(int status, byte[] body) {}
    public enum Operation { HEALTH, FEED, GET, CREATE, LIKE }
    private record Work(Operation op, String id, ByteView authorization, byte[] body, CompletableFuture<Reply> done) {}

    private final ArrayBlockingQueue<Work> queue = new ArrayBlockingQueue<>(4096);
    private final Connection connection;
    private final Statement control;
    private final PreparedStatement health, feed, get, create, like, exists;
    private final Thread owner;
    private final Input input;
    private final long started = System.nanoTime();
    private volatile boolean accepting = true;

    public Database(String path, Tokens tokens) {
        if (!Files.isRegularFile(Path.of(path))) throw new IllegalArgumentException("SQLITE_PATH must exist");
        Connection opened = null;
        try {
            opened = DriverManager.getConnection("jdbc:sqlite:" + Path.of(path).toAbsolutePath());
            connection = opened;
            control = connection.createStatement();
            for (String pragma : new String[]{
                    "locking_mode=EXCLUSIVE", "journal_mode=WAL", "synchronous=NORMAL",
                    "foreign_keys=ON", "busy_timeout=250", "mmap_size=1073741824",
                    "cache_size=500", "temp_store=MEMORY", "wal_autocheckpoint=1000"}) {
                control.execute("PRAGMA " + pragma);
            }
            input = new Input(connection, tokens);
            health = connection.prepareStatement("SELECT 1");
            feed = connection.prepareStatement(Sql.FEED);
            get = connection.prepareStatement(Sql.GET);
            create = connection.prepareStatement(Sql.CREATE);
            like = connection.prepareStatement(Sql.LIKE);
            exists = connection.prepareStatement("SELECT 1 FROM posts WHERE id=?");
        } catch (SQLException e) {
            if (opened != null) try { opened.close(); } catch (SQLException suppressed) { e.addSuppressed(suppressed); }
            throw new IllegalStateException("Cannot open SQLite", e);
        }
        owner = Thread.ofPlatform().name("sqlite-owner").start(this::run);
    }

    // The handler blocks until completion: borrowed header bytes cannot outlive their request.
    public Reply execute(Operation op, String id, ByteView authorization, byte[] body) {
        var done = new CompletableFuture<Reply>();
        synchronized (queue) {
            if (!accepting || !queue.offer(new Work(op, id, authorization, body, done)))
                return failure(op);
        }
        return done.join();
    }

    private void run() {
        var batch = new ArrayList<Work>(64);
        var answers = new ArrayList<Reply>(64);
        try {
            while (accepting || !queue.isEmpty()) {
                Work first = queue.poll(100, TimeUnit.MILLISECONDS);
                if (first == null) continue;
                batch.add(first);
                queue.drainTo(batch, 63);
                boolean writes = false;
                for (Work work : batch) {
                    if (work.op == Operation.CREATE || work.op == Operation.LIKE) { writes = true; break; }
                }
                try {
                    if (writes) control.execute("BEGIN IMMEDIATE");
                    for (Work work : batch) {
                        try {
                            answers.add(query(work));
                        } catch (HttpException e) {
                            answers.add(new Reply(e.status(), Json.error(e.getMessage())));
                        } catch (SQLException e) {
                            answers.add(failure(work.op));
                        }
                    }
                    if (writes) control.execute("COMMIT");
                    for (int i = 0; i < batch.size(); i++) batch.get(i).done.complete(answers.get(i));
                } catch (SQLException e) {
                    try { control.execute("ROLLBACK"); } catch (SQLException ignored) { }
                    for (Work work : batch) work.done.complete(failure(work.op));
                    LoggerFactory.getLogger(Database.class).warn("SQLite batch rolled back: {}", e.getErrorCode());
                }
                // Keep the active batch visible to the fatal-error handler until all replies complete.
                batch.clear();
                answers.clear();
            }
        } catch (Throwable failure) {
            synchronized (queue) {
                accepting = false;
                for (Work work : batch) work.done.complete(failure(work.op));
                Work pending;
                while ((pending = queue.poll()) != null) pending.done.complete(failure(pending.op));
            }
            LoggerFactory.getLogger(Database.class).error("SQLite owner stopped", failure);
        } finally {
            try { connection.close(); } catch (SQLException e) {
                LoggerFactory.getLogger(Database.class).warn("SQLite close failed", e);
            }
        }
    }

    private Reply query(Work work) throws SQLException {
        return switch (work.op) {
            case HEALTH -> {
                try (var rows = health.executeQuery()) {
                    if (!rows.next() || rows.getInt(1) != 1) yield degraded();
                }
                yield text(200, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"
                        + TimeUnit.NANOSECONDS.toSeconds(System.nanoTime() - started) + "}");
            }
            case FEED -> {
                try (var rows = feed.executeQuery()) {
                    if (!rows.next()) throw new SQLException("No aggregate");
                    yield new Reply(200, rows.getBytes(1));
                }
            }
            case GET -> {
                get.setLong(1, id(work.id));
                try (var rows = get.executeQuery()) {
                    yield rows.next() ? new Reply(200, rows.getBytes(1))
                            : new Reply(404, Json.error("post not found"));
                }
            }
            case CREATE -> {
                var user = input.authenticate(work.authorization);
                byte[] body = input.postBody(work.body);
                create.setLong(1, user.id());
                create.setBytes(2, body);
                create.setBytes(3, user.name());
                try (var rows = create.executeQuery()) {
                    if (!rows.next()) throw new SQLException("Insert returned no row");
                    byte[] bytes = rows.getBytes(1);
                    if (rows.next()) throw new SQLException("Insert returned multiple rows");
                    yield new Reply(201, bytes);
                }
            }
            case LIKE -> {
                var user = input.authenticate(work.authorization);
                long id = id(work.id);
                like.setLong(1, user.id());
                like.setLong(2, id);
                like.setLong(3, id);
                boolean inserted = like.executeUpdate() == 1;
                if (!inserted) {
                    exists.setLong(1, id);
                    try (var rows = exists.executeQuery()) {
                        if (!rows.next()) yield new Reply(404, Json.error("post not found"));
                    }
                }
                yield text(inserted ? 201 : 200, "{\"liked\":true,\"already_liked\":"
                        + !inserted + ",\"post_id\":" + id + "}");
            }
        };
    }

    private static long id(String text) {
        long id = Json.positiveId(text);
        if (id < 0) throw new HttpException(400, "invalid post id");
        return id;
    }

    private static Reply text(int status, String body) {
        return new Reply(status, body.getBytes(StandardCharsets.UTF_8));
    }

    private static Reply degraded() {
        return text(503, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database unavailable\"}");
    }

    private static Reply failure(Operation op) {
        if (op == Operation.HEALTH) return degraded();
        return new Reply(500, Json.error("internal server error"));
    }

    @Override public void close() throws InterruptedException {
        synchronized (queue) { accepting = false; }
        owner.join();
    }
}
