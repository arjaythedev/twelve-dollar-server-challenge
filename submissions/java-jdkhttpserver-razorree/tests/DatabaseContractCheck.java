package challenge;

import java.lang.reflect.*;
import java.nio.file.*;
import java.sql.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public final class DatabaseContractCheck {
    private static final Auth.User USER = new Auth.User(1, "golden_ember_1");

    private static final class Control implements Database.Opener {
        final AtomicBoolean blockFirst = new AtomicBoolean();
        final AtomicBoolean failCommit = new AtomicBoolean();
        final AtomicBoolean failRollback = new AtomicBoolean();
        final AtomicBoolean rollbackFailed = new AtomicBoolean();
        final AtomicBoolean unsafeAutoCommit = new AtomicBoolean();
        final AtomicInteger openResults = new AtomicInteger();
        final AtomicInteger writes = new AtomicInteger();
        final AtomicInteger maxBatch = new AtomicInteger();
        final CountDownLatch committing = new CountDownLatch(1);
        final CountDownLatch release = new CountDownLatch(1);

        @Override public Sqlite.Conn open(String url, Properties properties) throws SQLException {
            Sqlite.Conn real = Sqlite.open(url, properties);
            return (Sqlite.Conn) Proxy.newProxyInstance(getClass().getClassLoader(), new Class<?>[]{Sqlite.Conn.class}, (proxy, method, args) -> {
                String name = method.getName();
                if (name.equals("setAutoCommit") && Boolean.TRUE.equals(args[0]) && rollbackFailed.get()) {
                    unsafeAutoCommit.set(true);
                    throw new SQLException("Auto-commit after failed rollback");
                }
                if (name.equals("commit")) {
                    if (openResults.get() != 0) throw new AssertionError("RETURNING result still open at commit");
                    if (blockFirst.getAndSet(false)) {
                        committing.countDown();
                        if (!release.await(10, TimeUnit.SECONDS)) throw new SQLException("Commit gate timed out");
                    }
                    if (failCommit.getAndSet(false)) throw new SQLException("injected commit failure");
                    maxBatch.accumulateAndGet(writes.getAndSet(0), Math::max);
                }
                if (name.equals("rollback") && failRollback.getAndSet(false)) {
                    rollbackFailed.set(true);
                    throw new SQLException("injected rollback failure");
                }
                Object result = invoke(real, method, args);
                if (name.equals("prepareStatement") && ((String) args[0]).startsWith("INSERT"))
                    return statement((Sqlite.Stmt) result);
                return result;
            });
        }

        private Sqlite.Stmt statement(Sqlite.Stmt real) {
            return (Sqlite.Stmt) Proxy.newProxyInstance(getClass().getClassLoader(), new Class<?>[]{Sqlite.Stmt.class}, (proxy, method, args) -> {
                if (method.getName().equals("executeQuery") || method.getName().equals("executeUpdate")) writes.incrementAndGet();
                Object result = invoke(real, method, args);
                if (!(result instanceof Sqlite.Rows rows)) return result;
                openResults.incrementAndGet();
                AtomicBoolean closed = new AtomicBoolean();
                return Proxy.newProxyInstance(getClass().getClassLoader(), new Class<?>[]{Sqlite.Rows.class}, (rp, rm, ra) -> {
                    if (rm.getName().equals("close") && closed.compareAndSet(false, true)) openResults.decrementAndGet();
                    return invoke(rows, rm, ra);
                });
            });
        }
    }

    private static Object invoke(Object target, Method method, Object[] args) throws Throwable {
        try { return method.invoke(target, args); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }

    // A separate process avoids loading two independently linked SQLite libraries in this JVM.
    // https://www.sqlite.org/howtocorrupt.html#multiple_copies_of_sqlite_linked_into_the_same_application
    private static String observe(Path db, String sql, String... parameters) throws Exception {
        List<String> command = new ArrayList<>(List.of("python3", "-c",
            "import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); r=c.execute(sys.argv[2],sys.argv[3:]).fetchone(); print('' if r is None else r[0],end='')",
            db.toString(), sql));
        command.addAll(List.of(parameters));
        Process process = new ProcessBuilder(command).redirectError(ProcessBuilder.Redirect.INHERIT).start();
        if (!process.waitFor(60, TimeUnit.SECONDS) || process.exitValue() != 0) {
            process.destroyForcibly();
            throw new AssertionError("External database observer failed");
        }
        return new String(process.getInputStream().readAllBytes(), java.nio.charset.StandardCharsets.UTF_8);
    }

    private static int count(Path db, String body) throws Exception {
        return Integer.parseInt(observe(db, "SELECT count(*) FROM posts WHERE body=?", body));
    }

    private static void failed(Future<?> task) throws Exception {
        try { task.get(10, TimeUnit.SECONDS); throw new AssertionError("Failed write acknowledged"); }
        catch (ExecutionException expected) {
            if (!(expected.getCause() instanceof SQLException)) throw expected;
        }
    }

    private static void batching(Path path) throws Exception {
        Control control = new Control();
        control.blockFirst.set(true);
        try (Database db = new Database(path.toString(), control);
             ExecutorService callers = Executors.newVirtualThreadPerTaskExecutor()) {
            Future<Database.Post> first = callers.submit(() -> db.create(USER, "check-before-commit"));
            if (!control.committing.await(10, TimeUnit.SECONDS)) throw new AssertionError("No commit reached");
            if (first.isDone() || count(path, "check-before-commit") != 0)
                throw new AssertionError("Write acknowledged or visible before commit");
            List<Future<Database.Post>> tasks = new ArrayList<>();
            CountDownLatch ready = new CountDownLatch(301);
            for (int i = 0; i < 300; i++) {
                final String body = "check-batch-" + i;
                tasks.add(callers.submit(() -> { ready.countDown(); return db.create(USER, body); }));
            }
            Future<?> invalid = callers.submit(() -> {
                ready.countDown();
                return db.create(new Auth.User(Long.MAX_VALUE, "missing-user"), "check-bad-foreign-key");
            });
            if (!ready.await(10, TimeUnit.SECONDS)) throw new AssertionError("Concurrent submissions stalled");
            control.release.countDown();
            first.get(10, TimeUnit.SECONDS);
            for (Future<Database.Post> task : tasks) {
                Database.Post post = task.get(10, TimeUnit.SECONDS);
                String actual = observe(path, "SELECT body FROM posts WHERE id=?", Long.toString(post.id()));
                if (!post.body().equals(actual))
                    throw new AssertionError("Acknowledged write " + post.id() + " expected " + post.body() + " actual " + actual);
                if (!db.post(post.id()).body().equals(post.body()))
                    throw new AssertionError("Read did not observe acknowledged write");
            }
            failed(invalid);
            if (count(path, "check-bad-foreign-key") != 0) throw new AssertionError("Bad write persisted");
            if (control.maxBatch.get() <= 1 || control.maxBatch.get() > 256)
                throw new AssertionError("Batch outside 2..256: " + control.maxBatch);
            db.health();
        }
    }

    private static void failure(Path path, boolean rollback) throws Exception {
        Control control = new Control();
        control.failCommit.set(true);
        control.failRollback.set(rollback);
        String body = rollback ? "check-failed-rollback" : "check-failed-commit";
        try (Database db = new Database(path.toString(), control);
             ExecutorService callers = Executors.newVirtualThreadPerTaskExecutor()) {
            failed(callers.submit(() -> db.create(USER, body)));
            try { db.health(); throw new AssertionError("Uncertain connection still healthy"); }
            catch (SQLException expected) {}
        }
        if (control.unsafeAutoCommit.get()) throw new AssertionError("Enabled auto-commit after failed rollback");
        if (count(path, body) != 0) throw new AssertionError("Failed write committed during cleanup");
        try (Database db = new Database(path.toString())) {
            Database.Post post = db.create(USER, body + "-recovered");
            if (!db.post(post.id()).body().equals(body + "-recovered")) throw new AssertionError("Restart recovery failed");
        }
    }

    private static void readVisibilityDuringWrite(Path path) throws Exception {
        Control control = new Control();
        control.blockFirst.set(true);
        try (Database db = new Database(path.toString(), control);
             ExecutorService callers = Executors.newVirtualThreadPerTaskExecutor()) {
            byte[] before = db.feedJson();
            Future<Database.Post> write = callers.submit(() -> db.create(USER, "read-after-commit"));
            if (!control.committing.await(10, TimeUnit.SECONDS)) throw new AssertionError("No commit reached");
            Future<byte[]> read = callers.submit(db::feedJson);
            Thread.sleep(100);
            if (read.isDone()) throw new AssertionError("Read overtook uncommitted write on shared connection");
            control.release.countDown();
            Database.Post created = write.get(10, TimeUnit.SECONDS);
            byte[] response = read.get(10, TimeUnit.SECONDS);
            if (Arrays.equals(before, response))
                throw new AssertionError("Committed write did not change the feed");
            if (!new String(response, java.nio.charset.StandardCharsets.UTF_8).contains("read-after-commit"))
                throw new AssertionError("Committed write absent from feed");
            if (!Arrays.equals(db.postJson(created.id()), Json.encode(Json.object("post", db.post(created.id())))))
                throw new AssertionError("Committed post response differs from SQLite row");
        } finally { control.release.countDown(); }
    }

    private static void shutdownWithActiveRead(Path path) throws Exception {
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        AtomicInteger connections = new AtomicInteger(), activeCalls = new AtomicInteger();
        AtomicBoolean blockOnce = new AtomicBoolean(true);
        Database.Opener opener = (url, properties) -> {
            Sqlite.Conn real = Sqlite.open(url, properties);
            connections.incrementAndGet();
            return (Sqlite.Conn) Proxy.newProxyInstance(DatabaseContractCheck.class.getClassLoader(),
                new Class<?>[]{Sqlite.Conn.class}, (proxy, method, args) -> {
                    Object result = invoke(real, method, args);
                    if (!method.getName().equals("prepareStatement") ||
                        !((String) args[0]).contains("ORDER BY p.created_at")) return result;
                    Sqlite.Stmt statement = (Sqlite.Stmt) result;
                    return Proxy.newProxyInstance(DatabaseContractCheck.class.getClassLoader(),
                        new Class<?>[]{Sqlite.Stmt.class}, (sp, sm, sa) -> {
                            Object rows = invoke(statement, sm, sa);
                            if (!sm.getName().equals("executeQuery")) return rows;
                            return Proxy.newProxyInstance(DatabaseContractCheck.class.getClassLoader(),
                                new Class<?>[]{Sqlite.Rows.class}, (rp, rm, ra) -> {
                                    if (!rm.getName().equals("next")) return invoke(rows, rm, ra);
                                    if (activeCalls.incrementAndGet() != 1)
                                        throw new AssertionError("Concurrent native calls on one connection");
                                    try {
                                        if (blockOnce.getAndSet(false)) {
                                            entered.countDown();
                                            if (!release.await(10, TimeUnit.SECONDS))
                                                throw new SQLException("Read gate timed out");
                                        }
                                        return invoke(rows, rm, ra);
                                    } finally { activeCalls.decrementAndGet(); }
                                });
                        });
                });
        };
        Database db = new Database(path.toString(), opener);
        try (ExecutorService callers = Executors.newVirtualThreadPerTaskExecutor()) {
            Future<byte[]> first = callers.submit(db::feedJson);
            if (!entered.await(10, TimeUnit.SECONDS)) throw new AssertionError("Read did not reach gate");
            Future<byte[]> second = callers.submit(db::feedJson);
            Future<?> shutdown = callers.submit(() -> { db.close(); return null; });
            Thread.sleep(100);
            if (shutdown.isDone() || first.isDone() || second.isDone())
                throw new AssertionError("Shutdown or second read overtook active read");
            release.countDown();
            first.get(10, TimeUnit.SECONDS);
            shutdown.get(10, TimeUnit.SECONDS);
            try { second.get(10, TimeUnit.SECONDS); }
            catch (ExecutionException expected) {
                if (!(expected.getCause() instanceof SQLException)) throw expected;
            }
        } finally { release.countDown(); db.close(); }
        if (connections.get() != 1) throw new AssertionError("Expected one locked SQLite connection");
    }

    private static void shutdown(Path path) throws Exception {
        Control control = new Control();
        control.blockFirst.set(true);
        Database db = new Database(path.toString(), control);
        try (ExecutorService callers = Executors.newVirtualThreadPerTaskExecutor();
             ScheduledExecutorService release = Executors.newSingleThreadScheduledExecutor()) {
            Future<Database.Post> first = callers.submit(() -> db.create(USER, "check-shutdown-in-flight"));
            if (!control.committing.await(10, TimeUnit.SECONDS)) throw new AssertionError("No in-flight write");
            List<Future<Database.Post>> pending = new ArrayList<>();
            for (int i = 0; i < 64; i++) {
                final int n = i;
                pending.add(callers.submit(() -> db.create(USER, "check-shutdown-" + n)));
            }
            release.schedule(control.release::countDown, 100, TimeUnit.MILLISECONDS);
            db.close();
            first.get(10, TimeUnit.SECONDS);
            for (Future<Database.Post> task : pending) {
                try {
                    Database.Post post = task.get(10, TimeUnit.SECONDS);
                    if (count(path, post.body()) != 1) throw new AssertionError("Shutdown acknowledged uncommitted write");
                } catch (ExecutionException expected) {
                    if (!(expected.getCause() instanceof SQLException)) throw expected;
                }
            }
        } finally {
            control.release.countDown();
            db.close();
        }
    }

    private static void nativeCleanup(Path path) throws Exception {
        for (int i = 0; i < 20; i++) {
            try (Sqlite.Conn db = Sqlite.open(path.toString(), new Properties());
                 Sqlite.Stmt query = db.prepareStatement("SELECT ?")) {
                query.setString(1, "native\0text 😀");
                try (Sqlite.Rows rows = query.executeQuery()) {
                    if (!rows.next() || !"native\0text 😀".equals(rows.getString(1)))
                        throw new AssertionError("Native text byte-length copy failed");
                }
            }
        }
        try (Sqlite.Conn second = Sqlite.open(path.toString(), new Properties())) {
            try (Sqlite.Conn first = Sqlite.open(path.toString(), new Properties())) {
                try (Sqlite.Stmt query = first.prepareStatement("SELECT 1"); Sqlite.Rows rows = query.executeQuery()) {
                    if (!rows.next() || rows.getInt(1) != 1) throw new AssertionError("First native connection failed");
                }
            }
            try (Sqlite.Stmt query = second.prepareStatement("SELECT 1"); Sqlite.Rows rows = query.executeQuery()) {
                if (!rows.next() || rows.getInt(1) != 1) throw new AssertionError("Library unloaded with live connection");
            }
        }
        try { Sqlite.open(path.resolveSibling("absent-parent").resolve("fail.db").toString(), new Properties());
            throw new AssertionError("Missing parent opened");
        } catch (SQLException expected) {}
        Path empty = path.resolveSibling("partial-startup.db");
        Files.write(empty, new byte[0]);
        try { new Database(empty.toString()); throw new AssertionError("Missing schema opened"); }
        catch (SQLException expected) {}
        if (Files.readString(Path.of("/proc/self/maps")).contains("target/native/libsqlite3.so"))
            throw new AssertionError("Native library still loaded after all connections closed");
        System.out.println("PASS native text lengths, repeated close, failed open and partial startup cleanup");
    }

    private static void feedAlwaysQueriesSqlite(Path path) throws Exception {
        AtomicInteger executions = new AtomicInteger();
        Database.Opener opener = (url, properties) -> {
            Sqlite.Conn real = Sqlite.open(url, properties);
            return (Sqlite.Conn) Proxy.newProxyInstance(DatabaseContractCheck.class.getClassLoader(),
                new Class<?>[]{Sqlite.Conn.class}, (proxy, method, args) -> {
                    Object result = invoke(real, method, args);
                    if (!method.getName().equals("prepareStatement") ||
                        !((String) args[0]).contains("ORDER BY p.created_at")) return result;
                    Sqlite.Stmt statement = (Sqlite.Stmt) result;
                    return Proxy.newProxyInstance(DatabaseContractCheck.class.getClassLoader(),
                        new Class<?>[]{Sqlite.Stmt.class}, (sp, sm, sa) -> {
                            if (sm.getName().equals("executeQuery")) executions.incrementAndGet();
                            return invoke(statement, sm, sa);
                        });
                });
        };
        try (Database db = new Database(path.toString(), opener)) {
            byte[] first = db.feedJson();
            byte[] second = db.feedJson();
            if (executions.get() != 2) throw new AssertionError("Each feed must query SQLite");
            if (!Arrays.equals(first, second)) throw new AssertionError("Unchanged feed bytes differ");
        }
    }

    private static void realJsonRows(Path path) throws Exception {
        try (Database db = new Database(path.toString())) {
            for (String body : List.of("quote\" slash\\ nul\0 newline\n tab\t café 😀 𐀀", "\u0001\u001f\b\f\r")) {
                Database.Post created = db.create(USER, body);
                Database.Post observed = db.post(created.id());
                byte[] expected = Json.encode(Json.object("post", observed));
                if (!Arrays.equals(db.postJson(created.id()), expected))
                    throw new AssertionError("Real SQLite post bytes differ from JSON oracle");
                byte[] feed = db.feedJson();
                if (!Arrays.equals(feed, Json.encode(Json.object("posts", db.feed()))))
                    throw new AssertionError("Real SQLite feed bytes differ from JSON oracle");
                byte[] retained = db.postJson(created.id());
                db.feedJson();
                if (!Arrays.equals(retained, expected))
                    throw new AssertionError("Returned response bytes changed after buffer reuse");
                if (db.like(USER.id(), created.id()) != 1)
                    throw new AssertionError("Expected first like to change the post");
                if (!Arrays.equals(db.feedJson(), Json.encode(Json.object("posts", db.feed()))))
                    throw new AssertionError("Liked post was not refreshed in feed");
            }
        }
    }

    public static void main(String[] args) throws Exception {
        Path path = Path.of(args[1]);
        Files.createDirectories(path.getParent());
        Files.copy(Path.of(args[0]), path, StandardCopyOption.REPLACE_EXISTING);
        batching(path);
        readVisibilityDuringWrite(path);
        failure(path, false);
        failure(path, true);
        shutdown(path);
        shutdownWithActiveRead(path);
        feedAlwaysQueriesSqlite(path);
        realJsonRows(path);
        if (!"ok".equals(observe(path, "PRAGMA integrity_check"))) throw new AssertionError("SQLite integrity failure");
        nativeCleanup(path);
        System.out.println("PASS group commit, savepoints, acknowledgment ordering, injected commit/rollback failures and shutdown");
    }
}
