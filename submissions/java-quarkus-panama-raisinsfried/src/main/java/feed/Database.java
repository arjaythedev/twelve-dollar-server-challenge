package feed;

import io.quarkus.logging.Log;
import io.quarkus.runtime.Startup;
import io.smallrye.mutiny.Uni;
import io.smallrye.mutiny.subscription.UniEmitter;
import io.vertx.core.Context;
import io.vertx.core.Vertx;
import jakarta.annotation.PreDestroy;
import jakarta.inject.Singleton;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.LinkedBlockingDeque;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.locks.LockSupport;
import org.eclipse.microprofile.config.inject.ConfigProperty;

/**
 * Reads run on the caller's event loop on a LIFO pool of read connections, so most reads hit one warm cache. Writes are
 * group-committed by one writer thread: a batch is one transaction and one fsync, and each write's Uni completes on
 * the caller's context after the COMMIT returns. A failed statement fails only its own write (SQLite rolls it back);
 * a failed transaction fails the batch.
 */
@Startup
@Singleton
final class Database {
    private static final int MAX_BATCH = 256;
    /** Like Postgres's commit_delay: writes arriving meanwhile share the commit's fsync. */
    private static final long COMMIT_DELAY_NANOS = 2_000_000;

    interface Query<T> {
        T run(NativeDb db) throws Throwable;
    }

    private final LinkedBlockingDeque<NativeDb> readers = new LinkedBlockingDeque<>();
    private final List<NativeDb> all = new ArrayList<>();
    private final NativeDb writer;
    private final LinkedBlockingQueue<Write<?>> writes = new LinkedBlockingQueue<>();
    private final Thread writerThread = new Thread(this::writeLoop, "sqlite-writer");

    Database(@ConfigProperty(name = "feed.sqlite-path") String path) {
        int readConnections = Runtime.getRuntime().availableProcessors() + 1; // one per event loop + a spare
        try {
            for (int i = 0; i < readConnections; i++) readers.add(open(path));
            writer = open(path);
        } catch (Throwable t) {
            throw Sqlite.rethrow(t);
        }
        writerThread.setDaemon(true);
        writerThread.start();
        Log.infof("SQLite %s (%s): %d read connections + 1 write connection", Sqlite.version(), Sqlite.LIBRARY,
                readConnections);
    }

    private NativeDb open(String path) throws Throwable {
        NativeDb db = new NativeDb(path);
        all.add(db);
        return db;
    }

    <T> T read(Query<T> query) {
        try {
            NativeDb db = readers.takeFirst();
            try {
                return query.run(db);
            } finally {
                readers.addFirst(db);
            }
        } catch (Throwable t) {
            throw Sqlite.rethrow(t);
        }
    }

    <T> Uni<T> write(Query<T> query) {
        Context context = Vertx.currentContext();
        return Uni.createFrom().emitter(emitter -> writes.add(new Write<>(query, emitter, context)));
    }

    private void writeLoop() {
        var batch = new ArrayList<Write<?>>(MAX_BATCH);
        while (true) {
            try {
                batch.add(writes.take());
            } catch (InterruptedException e) {
                return;
            }
            LockSupport.parkNanos(COMMIT_DELAY_NANOS);
            writes.drainTo(batch, MAX_BATCH - 1);
            Throwable failure = null;
            try {
                writer.begin();
                for (Write<?> w : batch) w.run(writer);
                writer.commit();
            } catch (Throwable t) {
                failure = t;
                try {
                    writer.rollback();
                } catch (Throwable rollback) {
                    t.addSuppressed(rollback);
                }
            }
            for (Write<?> w : batch) w.finish(failure);
            batch.clear();
        }
    }

    private static final class Write<T> {
        final Query<T> query;
        final UniEmitter<? super T> emitter;
        final Context context;
        T result;
        Throwable error;

        Write(Query<T> query, UniEmitter<? super T> emitter, Context context) {
            this.query = query;
            this.emitter = emitter;
            this.context = context;
        }

        /** Rethrows only if SQLite rolled back the whole transaction. */
        void run(NativeDb db) throws Throwable {
            try {
                result = query.run(db);
            } catch (Throwable t) {
                error = t;
                if (db.autocommit()) throw t;
            }
        }

        void finish(Throwable batchFailure) {
            Throwable failure = batchFailure != null ? batchFailure : error;
            Runnable done = () -> {
                if (failure == null) emitter.complete(result);
                else emitter.fail(failure);
            };
            if (context == null) done.run();
            else context.runOnContext(ignored -> done.run());
        }
    }

    @PreDestroy
    void close() {
        writerThread.interrupt();
        try {
            writerThread.join(5000);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        all.forEach(NativeDb::close);
    }
}
