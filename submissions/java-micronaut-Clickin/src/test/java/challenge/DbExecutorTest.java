package challenge;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import java.nio.file.Path;
import java.sql.*;
import java.util.ArrayList;
import java.util.concurrent.*;
import static org.junit.jupiter.api.Assertions.*;

class DbExecutorTest {
    @TempDir Path directory;

    private Connection database() throws SQLException {
        var connection = DriverManager.getConnection("jdbc:sqlite:" + directory.resolve("batch.db"));
        try (var statement = connection.createStatement()) {
            statement.execute("PRAGMA journal_mode=WAL");
            statement.execute("PRAGMA foreign_keys=ON");
            statement.execute("CREATE TABLE parents(id INTEGER PRIMARY KEY)");
            statement.execute("INSERT INTO parents VALUES(1)");
            statement.execute("CREATE TABLE writes(id INTEGER PRIMARY KEY,parent INTEGER REFERENCES parents(id) DEFERRABLE INITIALLY DEFERRED)");
        }
        return connection;
    }
    private static int sql(Connection connection, String sql) {
        try (var statement = connection.createStatement()) { return statement.executeUpdate(sql); }
        catch (SQLException e) { throw new IllegalStateException(e); }
    }
    private static int count(Connection connection) {
        try (var statement = connection.createStatement(); var rows = statement.executeQuery("SELECT count(*) FROM writes")) {
            rows.next(); return rows.getInt(1);
        } catch (SQLException e) { throw new IllegalStateException(e); }
    }
    private record Gate(CountDownLatch release) implements AutoCloseable {
        public void close() { release.countDown(); }
    }
    private static Gate blockOwner(DbExecutor executor) throws Exception {
        var release = new CountDownLatch(1);
        var started = new CountDownLatch(1);
        executor.read(() -> {
            started.countDown();
            try { if (!release.await(5, TimeUnit.SECONDS)) throw new IllegalStateException("Gate timed out"); }
            catch (InterruptedException e) { throw new IllegalStateException(e); }
            return null;
        });
        assertTrue(started.await(5, TimeUnit.SECONDS));
        return new Gate(release);
    }
    private static void fails(CompletableFuture<?> future, int status) {
        var failure = assertThrows(ExecutionException.class, () -> future.get(5, TimeUnit.SECONDS));
        assertEquals(status, assertInstanceOf(ApiException.class, failure.getCause()).status);
    }

    @Test void publishesReadsAndWritesOnlyAfterCommitAndRollsBackFailedBatches() throws Exception {
        try (var writer = database(); var observer = DriverManager.getConnection("jdbc:sqlite:" + directory.resolve("batch.db"))) {
            var executor = new DbExecutor();
            executor.start(writer);
            try {
                CompletableFuture<Integer> first, second, read;
                try (var gate = blockOwner(executor)) {
                    first = executor.write(() -> sql(writer, "INSERT INTO writes VALUES(1,1)"));
                    second = executor.write(() -> sql(writer, "INSERT INTO writes VALUES(2,1)"));
                    var pendingFirst = first;
                    var pendingSecond = second;
                    read = executor.read(() -> {
                        assertEquals(0, count(observer));
                        assertFalse(pendingFirst.isDone());
                        assertFalse(pendingSecond.isDone());
                        return count(writer);
                    });
                }
                assertEquals(1, first.get(5, TimeUnit.SECONDS));
                assertEquals(1, second.get(5, TimeUnit.SECONDS));
                assertEquals(2, read.get(5, TimeUnit.SECONDS));
                assertEquals(2, count(observer));

                // Deferred foreign keys fail at COMMIT, after all suppliers have returned.
                try (var gate = blockOwner(executor)) {
                    first = executor.write(() -> sql(writer, "INSERT INTO writes VALUES(3,999)"));
                    second = executor.write(() -> sql(writer, "INSERT INTO writes VALUES(4,1)"));
                    read = executor.read(() -> count(writer));
                }
                fails(first, 500); fails(second, 500); fails(read, 500);
                assertEquals(2, count(observer));

                // A statement failure must also roll back earlier writes in the same batch.
                try (var gate = blockOwner(executor)) {
                    first = executor.write(() -> sql(writer, "INSERT INTO writes VALUES(5,1)"));
                    second = executor.write(() -> sql(writer, "INSERT INTO writes VALUES(1,1)"));
                }
                fails(first, 500); fails(second, 500);
                assertEquals(2, count(observer));
                assertEquals(1, executor.write(() -> sql(writer, "INSERT INTO writes VALUES(6,1)")).get(5, TimeUnit.SECONDS));
                assertEquals(3, count(observer));
            } finally { executor.close(); }
        }
    }

    @Test void stopsAdmissionWhenSQLiteHasAlreadyRolledBackTheTransaction() throws Exception {
        try (var writer = database()) {
            sql(writer, "INSERT INTO writes VALUES(1,1)");
            var executor = new DbExecutor();
            executor.start(writer);
            try {
                var pending = new ArrayList<CompletableFuture<Integer>>();
                try (var gate = blockOwner(executor)) {
                    pending.add(executor.write(() -> sql(writer, "INSERT OR ROLLBACK INTO writes VALUES(1,1)")));
                    for (int i = 2; i <= DbExecutor.BATCH_SIZE + 1; i++) {
                        int id = i;
                        pending.add(executor.write(() -> sql(writer, "INSERT INTO writes VALUES(" + id + ",1)")));
                    }
                }
                for (int i = 0; i < DbExecutor.BATCH_SIZE; i++) fails(pending.get(i), 500);
                fails(pending.getLast(), 503);
                executor.close();
                fails(executor.write(() -> sql(writer, "INSERT INTO writes VALUES(99999,1)")), 503);
                assertEquals(1, count(writer));
            } finally { executor.close(); }
        }
    }

    @Test void rejectsOverflowAndDrainsAllAcceptedWritesOnShutdown() throws Exception {
        try (var writer = database()) {
            var executor = new DbExecutor();
            executor.start(writer);
            var accepted = new ArrayList<CompletableFuture<Integer>>();
            try {
                try (var gate = blockOwner(executor)) {
                    for (int i = 1; i <= DbExecutor.CAPACITY; i++) {
                        int id = i;
                        accepted.add(executor.write(() -> sql(writer, "INSERT INTO writes VALUES(" + id + ",1)")));
                    }
                    fails(executor.write(() -> sql(writer, "INSERT INTO writes VALUES(99999,1)")), 503);
                }
                executor.close();
                for (var task : accepted) assertEquals(1, task.get(5, TimeUnit.SECONDS));
                assertEquals(DbExecutor.CAPACITY, count(writer));
                fails(executor.read(() -> count(writer)), 503);
            } finally { executor.close(); }
        }
    }
}
