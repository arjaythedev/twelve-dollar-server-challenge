package challenge;

import java.nio.charset.StandardCharsets;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import reactor.core.publisher.Mono;
import reactor.core.publisher.MonoSink;
import reactor.core.scheduler.Scheduler;
import reactor.core.scheduler.Schedulers;

/**
 * SQLite access, through the FFM binding in {@link Sqlite}.
 *
 * <p>Reads: one connection per thread (thread-local), each with its own prepared statements. With
 * {@code readers == 0} queries run inline on the calling Netty event-loop thread (every query is an
 * index lookup that takes well under a millisecond); otherwise they hop to a fixed pool of that size.
 *
 * <p>Writes: a single writer thread owns the only write connection and does group commit: it drains
 * every queued write into one {@code BEGIN IMMEDIATE ... COMMIT} and completes each request only after
 * that commit returns, so a 201 always means the row is committed.
 */
final class Db implements AutoCloseable {

    private static final Logger log = LoggerFactory.getLogger(Db.class);

    private static final String POST_COLUMNS =
            "p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)";
    // walks posts_created_at_id_idx newest-first and stops after 20 rows: no temp b-tree
    private static final String FEED_SQL = "SELECT " + POST_COLUMNS
            + " FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
    private static final String POST_SQL = "SELECT " + POST_COLUMNS
            + " FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?";
    private static final String INSERT_POST_SQL =
            "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at";
    private static final String INSERT_LIKE_SQL =
            "INSERT INTO likes (user_id, post_id) SELECT ?, ? WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?)"
                    + " ON CONFLICT (user_id, post_id) DO NOTHING";
    private static final String POST_EXISTS_SQL = "SELECT 1 FROM posts WHERE id = ?";

    @FunctionalInterface
    interface ReadOp<T> {
        T apply(Reader r) throws SQLException;
    }

    private final String path;
    private final ThreadLocal<Reader> reader = ThreadLocal.withInitial(this::openReader);
    private final ExecutorService readPool;
    private final Scheduler readScheduler;
    private final int maxBatch;
    private final LinkedBlockingQueue<WriteTask> queue = new LinkedBlockingQueue<>();
    private final Thread writer;

    Db(String path, int readers, int maxBatch) {
        this.path = path;
        this.maxBatch = Math.max(1, maxBatch);
        if (readers > 0) {
            readPool = Executors.newFixedThreadPool(readers, Thread.ofPlatform().daemon().name("db-read-", 0).factory());
            readScheduler = Schedulers.fromExecutorService(readPool, "db-read");
        } else {
            readPool = null;
            readScheduler = null;
        }
        Writer w = openWriter(); // fail fast at startup if the database can't be opened
        writer = Thread.ofPlatform().daemon().name("db-writer").start(() -> writerLoop(w));
    }

    private Sqlite.Conn open(boolean readOnly) throws SQLException {
        Sqlite.Conn c = new Sqlite.Conn(path, Sqlite.OPEN_READWRITE | Sqlite.OPEN_NOMUTEX);
        c.busyTimeout(5000);
        c.exec("PRAGMA journal_mode = WAL");
        c.exec("PRAGMA synchronous = NORMAL");
        c.exec("PRAGMA mmap_size = " + (1L << 30));
        // Reads come through mmap, so SQLite's own page cache only needs to hold the writer's dirty pages.
        c.exec("PRAGMA cache_size = -" + Integer.getInteger("app.sqliteCacheKb", 2048));
        if (readOnly) c.exec("PRAGMA query_only = ON");
        return c;
    }

    // ---------------------------------------------------------------- reads

    <T> Mono<T> read(ReadOp<T> op) {
        Mono<T> m = Mono.fromCallable(() -> op.apply(reader.get()));
        return readScheduler == null ? m : m.subscribeOn(readScheduler);
    }

    /** True when reads run on the calling thread, so callers may use {@link #readNow} and skip Reactor. */
    boolean inlineReads() {
        return readScheduler == null;
    }

    <T> T readNow(ReadOp<T> op) throws SQLException {
        return op.apply(reader.get());
    }

    private Reader openReader() {
        try {
            return new Reader(open(true));
        } catch (SQLException e) {
            throw new IllegalStateException("cannot open reader connection", e);
        }
    }

    static final class Reader {
        private final Sqlite.Stmt feed;
        private final Sqlite.Stmt post;
        private final Sqlite.Stmt ping;
        private final Out out = new Out();

        Reader(Sqlite.Conn c) throws SQLException {
            feed = c.prepare(FEED_SQL);
            post = c.prepare(POST_SQL);
            ping = c.prepare("SELECT 1");
        }

        void ping() throws SQLException {
            try {
                if (!ping.step()) throw new SQLException("SELECT 1 returned no row");
            } finally {
                ping.reset();
            }
        }

        Resp feed() throws SQLException {
            Out o = out.reset().ascii("{\"posts\":[");
            try {
                boolean first = true;
                while (feed.step()) {
                    if (!first) o.ch(',');
                    first = false;
                    postJson(o, feed);
                }
            } finally {
                feed.reset();
            }
            return o.ascii("]}").resp(200);
        }

        Resp post(long id) throws SQLException {
            try {
                if (!post.bind(1, id).step()) return Resp.POST_NOT_FOUND;
                Out o = out.reset().ascii("{\"post\":");
                postJson(o, post);
                return o.ch('}').resp(200);
            } finally {
                post.reset();
            }
        }

        /** Writes the current row of a POST_COLUMNS query as a post object, keys in spec order. */
        private static void postJson(Out o, Sqlite.Stmt s) {
            o.ascii("{\"id\":").num(s.getLong(0)).ascii(",\"body\":");
            s.textTo(1, o);
            o.ascii(",\"created_at\":");
            s.textTo(2, o);
            o.ascii(",\"author\":");
            s.textTo(3, o);
            o.ascii(",\"like_count\":").num(s.getLong(4)).ch('}');
        }
    }

    // ---------------------------------------------------------------- writes

    Mono<Resp> createPost(long userId, String username, String body) {
        return submit(w -> w.createPost(userId, username, body));
    }

    Mono<Resp> like(long userId, long postId) {
        return submit(w -> w.like(userId, postId));
    }

    @FunctionalInterface
    private interface WriteOp {
        Resp apply(Writer w) throws SQLException;
    }

    private static final class WriteTask {
        final WriteOp op;
        final MonoSink<Resp> sink;
        Resp result;

        WriteTask(WriteOp op, MonoSink<Resp> sink) {
            this.op = op;
            this.sink = sink;
        }
    }

    private Mono<Resp> submit(WriteOp op) {
        return Mono.create(sink -> queue.add(new WriteTask(op, sink)));
    }

    private Writer openWriter() {
        try {
            return new Writer(open(false));
        } catch (SQLException e) {
            throw new IllegalStateException("cannot open writer connection", e);
        }
    }

    private void writerLoop(Writer w) {
        ArrayList<WriteTask> batch = new ArrayList<>(maxBatch);
        while (true) {
            try {
                batch.add(queue.take());
            } catch (InterruptedException e) {
                return;
            }
            queue.drainTo(batch, maxBatch - 1);
            boolean tx = batch.size() > 1; // a lone write just autocommits
            try {
                if (tx) w.begin.run();
                for (WriteTask t : batch) {
                    try {
                        t.result = t.op.apply(w);
                    } catch (SQLException e) {
                        log.error("write failed", e);
                        t.result = Resp.INTERNAL_ERROR;
                    }
                }
                if (tx) w.commit.run();
            } catch (SQLException e) {
                log.error("group commit of {} writes failed", batch.size(), e);
                try {
                    w.rollback.run();
                } catch (SQLException ignored) {
                    // no transaction left to roll back
                }
                for (WriteTask t : batch) t.result = Resp.INTERNAL_ERROR;
            }
            for (WriteTask t : batch) t.sink.success(t.result);
            batch.clear();
        }
    }

    private static final class Writer {
        final Sqlite.Conn conn;
        final Sqlite.Stmt begin;
        final Sqlite.Stmt commit;
        final Sqlite.Stmt rollback;
        final Sqlite.Stmt insertPost;
        final Sqlite.Stmt insertLike;
        final Sqlite.Stmt postExists;
        final Out out = new Out();

        Writer(Sqlite.Conn c) throws SQLException {
            conn = c;
            begin = c.prepare("BEGIN IMMEDIATE");
            commit = c.prepare("COMMIT");
            rollback = c.prepare("ROLLBACK");
            insertPost = c.prepare(INSERT_POST_SQL);
            insertLike = c.prepare(INSERT_LIKE_SQL);
            postExists = c.prepare(POST_EXISTS_SQL);
        }

        Resp createPost(long userId, String username, String body) throws SQLException {
            byte[] utf8 = body.getBytes(StandardCharsets.UTF_8);
            try {
                if (!insertPost.bind(1, userId).bind(2, utf8).step()) {
                    throw new SQLException("INSERT ... RETURNING returned no row");
                }
                Out o = out.reset().ascii("{\"post\":{\"id\":").num(insertPost.getLong(0))
                        .ascii(",\"body\":").str(utf8).ascii(",\"created_at\":");
                insertPost.textTo(1, o);
                o.ascii(",\"author\":").str(username).ascii(",\"like_count\":0}}");
                while (insertPost.step()) {
                    // run the statement to completion so an autocommit write is committed before we answer
                }
                return o.resp(201);
            } finally {
                insertPost.reset();
            }
        }

        Resp like(long userId, long postId) throws SQLException {
            insertLike.bind(1, userId).bind(2, postId).bind(3, postId).run();
            boolean inserted = conn.changes() == 1;
            if (!inserted) {
                try {
                    if (!postExists.bind(1, postId).step()) return Resp.POST_NOT_FOUND;
                } finally {
                    postExists.reset();
                }
            }
            return out.reset().ascii(inserted ? "{\"liked\":true,\"already_liked\":false,\"post_id\":"
                            : "{\"liked\":true,\"already_liked\":true,\"post_id\":")
                    .num(postId).ch('}').resp(inserted ? 201 : 200);
        }
    }

    @Override
    public void close() {
        writer.interrupt();
        if (readPool != null) readPool.shutdownNow();
    }
}
