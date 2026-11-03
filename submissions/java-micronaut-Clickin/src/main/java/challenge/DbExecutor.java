package challenge;

import jakarta.inject.Singleton;
import jakarta.annotation.PreDestroy;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.concurrent.*;
import java.util.function.Function;
import java.util.function.Supplier;

@Singleton
public final class DbExecutor {
    private static final Logger LOG = LoggerFactory.getLogger(DbExecutor.class);
    static final int CAPACITY = 4096, BATCH_SIZE = 256;
    private static final Task<Void> STOP = new Task<>(false, () -> null, null);
    private final ArrayBlockingQueue<Task<?>> queue = new ArrayBlockingQueue<>(CAPACITY);
    private final Object admission = new Object();
    private boolean accepting;
    private Thread worker;
    private PreparedStatement begin, commit, rollback;

    void start(Connection connection) throws SQLException {
        begin = connection.prepareStatement("BEGIN IMMEDIATE");
        commit = connection.prepareStatement("COMMIT");
        rollback = connection.prepareStatement("ROLLBACK");
        synchronized (admission) {
            accepting = true;
            worker = Thread.ofPlatform().name("db-owner").start(this::run);
        }
    }

    private static final class Task<T> {
        final boolean write;
        final Supplier<T> action;
        /** Maps a failure to a value so the caller needs no second future; null completes the future exceptionally. */
        final Function<Throwable, T> onError;
        final CompletableFuture<T> future = new CompletableFuture<>();
        T value;
        ApiException error;
        Task(boolean write, Supplier<T> action, Function<Throwable, T> onError) {
            this.write = write; this.action = action; this.onError = onError;
        }
        void fail(Throwable failure) {
            if (onError == null) future.completeExceptionally(failure);
            else future.complete(onError.apply(failure));
        }
        void execute() {
            try { value = action.get(); }
            catch (ApiException e) {
                if (e.status >= 500) throw e;
                error = e;
            }
        }
        void complete() {
            if (error == null) future.complete(value);
            else fail(error);
        }
    }

    public <T> CompletableFuture<T> read(Supplier<T> action) { return submit(false, action, null); }
    public <T> CompletableFuture<T> write(Supplier<T> action) { return submit(true, action, null); }
    /** As {@link #read(Supplier)}, but a failure completes the single future with {@code onError(failure)}. */
    public <T> CompletableFuture<T> read(Supplier<T> action, Function<Throwable, T> onError) { return submit(false, action, onError); }
    public <T> CompletableFuture<T> write(Supplier<T> action, Function<Throwable, T> onError) { return submit(true, action, onError); }
    private <T> CompletableFuture<T> submit(boolean write, Supplier<T> action, Function<Throwable, T> onError) {
        var task = new Task<>(write, action, onError);
        synchronized (admission) {
            if (!accepting || !queue.offer(task))
                task.fail(new ApiException(503, "internal server error"));
        }
        return task.future;
    }

    private void run() {
        var batch = new ArrayList<Task<?>>(BATCH_SIZE);
        try {
            boolean stopping = false;
            while (!stopping) {
                batch.add(queue.take());
                // Only collect work already waiting: no timer or minimum batch size.
                queue.drainTo(batch, BATCH_SIZE - 1);
                if (batch.getLast() == STOP) {
                    batch.removeLast();
                    stopping = true;
                }
                boolean writes = false;
                for (var task : batch) writes |= task.write;
                boolean transaction = false;
                boolean usable = true;
                try {
                    if (writes) { begin.executeUpdate(); transaction = true; }
                    for (var task : batch) task.execute();
                    if (writes) { commit.executeUpdate(); transaction = false; }
                    // Reads in this batch may have seen its writes; publish neither before COMMIT.
                    for (var task : batch) task.complete();
                } catch (Throwable failure) {
                    if (transaction) {
                        try { rollback.executeUpdate(); }
                        catch (SQLException uncertain) {
                            failure.addSuppressed(uncertain);
                            usable = false;
                        }
                    }
                    LOG.error("SQLite batch failed", failure);
                    for (var task : batch)
                        task.fail(new ApiException(500, "internal server error"));
                    if (!usable || failure instanceof Error) break;
                }
                batch.clear();
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } finally {
            synchronized (admission) { accepting = false; }
            Task<?> task;
            while ((task = queue.poll()) != null)
                task.fail(new ApiException(503, "internal server error"));
        }
    }

    @PreDestroy public void close() {
        boolean interrupted = false;
        boolean stop;
        synchronized (admission) {
            stop = accepting;
            accepting = false;
        }
        if (stop) {
            while (true) {
                try { queue.put(STOP); break; }
                catch (InterruptedException e) { interrupted = true; }
            }
        }
        if (worker != null) {
            while (worker.isAlive()) {
                try { worker.join(); }
                catch (InterruptedException e) { interrupted = true; }
            }
        }
        if (interrupted) Thread.currentThread().interrupt();
    }
}
