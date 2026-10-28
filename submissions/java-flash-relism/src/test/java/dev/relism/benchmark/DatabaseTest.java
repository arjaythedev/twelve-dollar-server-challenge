package dev.relism.benchmark;

import dev.relism.benchmark.db.Database;
import dev.relism.benchmark.auth.Tokens;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.time.Duration;
import java.sql.DriverManager;
import java.util.ArrayList;
import java.util.concurrent.Callable;
import java.util.concurrent.Executors;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import static dev.relism.benchmark.db.Database.Operation.*;
import static org.junit.jupiter.api.Assertions.*;

class DatabaseTest {
    @TempDir Path directory;

    private Path fixture(boolean deferred) throws Exception {
        Path path = directory.resolve("feed.db");
        try (var conn = DriverManager.getConnection("jdbc:sqlite:" + path); var sql = conn.createStatement()) {
            sql.execute("CREATE TABLE users(id INTEGER PRIMARY KEY,username TEXT NOT NULL)");
            sql.execute("""
                    CREATE TABLE posts(id INTEGER PRIMARY KEY,user_id INTEGER REFERENCES users(id)%s,
                    body TEXT NOT NULL CHECK(length(body) BETWEEN 1 AND 500),
                    created_at TEXT NOT NULL DEFAULT(strftime('%%Y-%%m-%%dT%%H:%%M:%%fZ','now')))
                    """.formatted(deferred ? " DEFERRABLE INITIALLY DEFERRED" : ""));
            sql.execute("""
                    CREATE TABLE likes(user_id INTEGER REFERENCES users(id),post_id INTEGER REFERENCES posts(id),
                    PRIMARY KEY(user_id,post_id))
                    """);
            sql.execute("CREATE INDEX posts_created_at_id_idx ON posts(created_at DESC,id DESC)");
            sql.execute("CREATE INDEX likes_post_id_idx ON likes(post_id)");
            sql.execute("INSERT INTO users VALUES(1,'one'),(2,'two')");
            sql.execute("INSERT INTO posts(id,user_id,body,created_at) VALUES"
                    + "(500,1,'older','2025-01-01T00:00:00.000Z'),"
                    + "(7,1,'newer','2026-01-01T00:00:00.000Z'),"
                    + "(9,2,'tie','2026-01-01T00:00:00.000Z')");
        }
        return path;
    }

    @Test void concurrentLikesAreIdempotentAndReadsAreFresh() throws Exception {
        Path path = fixture(false);
        try (var db = new Database(path.toString(), new Tokens("secret")); var executor = Executors.newVirtualThreadPerTaskExecutor()) {
            var calls = new ArrayList<Callable<Database.Reply>>();
            for (int i = 0; i < 64; i++) calls.add(() -> call(db, LIKE, 7, 1, null, null));
            int created = 0;
            for (var result : executor.invokeAll(calls)) {
                int status = result.get().status();
                assertTrue(status == 200 || status == 201);
                if (status == 201) created++;
            }
            assertEquals(1, created);
            String feed = text(call(db, FEED, 0, 0, null, null));
            assertTrue(feed.indexOf("\"id\":9") < feed.indexOf("\"id\":7"));
            assertTrue(feed.indexOf("\"id\":7") < feed.indexOf("\"id\":500"));
            assertTrue(text(call(db, GET, 7, 0, null, null)).contains("\"like_count\":1"));
            assertEquals(201, call(db, CREATE, 0, 1, "new", "token-author").status());
            assertTrue(text(call(db, FEED, 0, 0, null, null)).contains("\"body\":\"new\""));
            assertEquals(500, call(db, CREATE, 0, 999, "invalid user", "bad").status());
            assertEquals(201, call(db, CREATE, 0, 1, "still working", "one").status());
        }
        try (var conn = DriverManager.getConnection("jdbc:sqlite:" + path); var sql = conn.createStatement();
             var result = sql.executeQuery("SELECT count(*) FROM likes")) {
            assertTrue(result.next());
            assertEquals(1, result.getInt(1));
        }
    }

    @Test void failedCommitNeverReturnsSuccessAndRollsBack() throws Exception {
        Path path = fixture(true);
        try (var db = new Database(path.toString(), new Tokens("secret"))) {
            assertEquals(500, call(db, CREATE, 0, 999, "must roll back", "bad").status());
            assertFalse(text(call(db, FEED, 0, 0, null, null)).contains("must roll back"));
            assertEquals(201, call(db, CREATE, 0, 1, "after rollback", "one").status());
        }
    }

    @Test void ownerFailureCompletesCurrentRequestAndStopsAccepting() throws Exception {
        try (var db = new Database(fixture(false).toString(), new Tokens("secret"))) {
            // Inject an internal invalid work item to exercise an unexpected owner exception.
            assertTimeoutPreemptively(Duration.ofSeconds(3),
                    () -> assertEquals(500, call(db, CREATE, 0, 1, null, "one").status()));
            assertEquals(503, call(db, HEALTH, 0, 0, null, null).status());
        }
    }

    @Test void closedDatabaseReportsDegradedHealth() throws Exception {
        var db = new Database(fixture(false).toString(), new Tokens("secret"));
        db.close();
        var health = call(db, HEALTH, 0, 0, null, null);
        assertEquals(503, health.status());
        assertEquals("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database unavailable\"}", text(health));
        assertEquals(500, call(db, FEED, 0, 0, null, null).status());
    }

    private static Database.Reply call(Database db, Database.Operation op, long id, long user, String body, String author) throws Exception {
        String token = null;
        if (op == CREATE || op == LIKE) {
            token = "Bearer " + ValidationTest.sign("secret", "{\"alg\":\"HS256\"}",
                    "{\"sub\":\"" + user + "\",\"username\":\"" + (author == null ? "one" : author)
                    + "\",\"exp\":" + (System.currentTimeMillis() / 1000 + 3600) + "}");
        }
        byte[] bytes = body == null ? null : ("{\"body\":\"" + body + "\"}").getBytes(StandardCharsets.UTF_8);
        return db.execute(op, Long.toString(id), ValidationTest.header(token), bytes);
    }

    private String text(Database.Reply reply) {
        return new String(reply.body(), StandardCharsets.UTF_8);
    }
}
