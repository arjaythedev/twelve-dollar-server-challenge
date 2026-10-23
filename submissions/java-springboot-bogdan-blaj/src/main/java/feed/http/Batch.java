package feed.http;

import feed.db.FeedDb;
import feed.db.Sqlite;
import io.netty.buffer.ByteBuf;
import io.netty.channel.ChannelFutureListener;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.EventLoop;
import io.netty.channel.SingleThreadEventLoop;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.util.Arrays;

/**
 * Group commit: at most one SQLite transaction per event-loop pass, and the order in which responses leave.
 *
 * <p>Netty's loop runs: wait for I/O, handle every ready channel, run queued tasks, run "after iteration" tail
 * tasks. A pass's first statement, read or write ({@link #begin}), executes {@code BEGIN}; from then on every
 * response of the pass, whatever produced it, is held in arrival order. {@link #runTail} (registered once per pass
 * as a tail task) commits, then writes and flushes everything held, then checkpoints if the WAL is large. A
 * response that needed no statement is written at once if nothing is held.
 *
 * <p><b>Durability invariant:</b> no success response for a write reaches Netty before the {@code COMMIT} of the
 * transaction that executed the write returned {@code SQLITE_DONE}. A write only runs after {@link #begin} opened
 * the transaction, so its response is held, and held responses are passed to Netty only by {@link #runTail} after
 * a successful {@code COMMIT}; on any failure they are replaced by 500 first. {@code HttpServerTest} checks this
 * on the wire.
 *
 * <p>Holding keeps per-connection order: a pipelined connection never sees a later response overtake a held one,
 * because nothing is written directly while anything is held.
 *
 * <p>Everything here runs on the event loop; no field needs synchronization.
 */
final class Batch {

    private static final Logger log = LoggerFactory.getLogger(Batch.class);
    private static final int INITIAL_HELD = 256;

    private final FeedDb db;
    private final SingleThreadEventLoop loop;
    private final int ckptFrames;
    private final long stallNanos;
    private final Runnable tail = this::runTail;
    private final RateLimit commitErrors = new RateLimit(10_000);
    private final RateLimit checkpointErrors = new RateLimit(10_000);

    private boolean open;
    private boolean scheduled;
    private boolean inTail;
    /** Index of the first held response produced inside the open transaction. */
    private int txStart;
    private int count;
    private ChannelHandlerContext[] ctxs = new ChannelHandlerContext[INITIAL_HELD];
    private ByteBuf[] bufs = new ByteBuf[INITIAL_HELD];
    private boolean[] closes = new boolean[INITIAL_HELD];
    private int lastCommitRc = Sqlite.SQLITE_DONE;

    private long passStartNanos;
    private int passRequests;
    private int passAccepts;

    private long commits;
    private long commitFailures;
    private long checkpoints;
    private long checkpointFailures;
    private long checkpointMaxNanos;

    /** Test seam: when true every COMMIT is replaced by a ROLLBACK and treated as failed. Not reachable from flags. */
    boolean failCommitsForTest;

    Batch(FeedDb db, EventLoop loop, FeedConfig config) {
        if (!(loop instanceof SingleThreadEventLoop stl)) {
            throw new IllegalArgumentException("group commit needs a SingleThreadEventLoop, got " + loop.getClass());
        }
        this.db = db;
        this.loop = stl;
        this.ckptFrames = config.ckptFrames();
        this.stallNanos = config.stallLogMs() * 1_000_000L;
    }

    /** Before any statement: joins the pass's transaction. False means BEGIN failed and the statement must not run. */
    boolean begin() {
        return open || openTransaction();
    }

    private boolean openTransaction() {
        try {
            db.begin();
        } catch (RuntimeException e) {
            if (commitErrors.tryAcquire()) {
                log.warn("BEGIN failed: {} ({} similar suppressed)", e.getMessage(), commitErrors.takeSuppressed());
            }
            return false;
        }
        open = true;
        txStart = count;
        schedule();
        return true;
    }

    /** Hands a complete response to the batch: written now if nothing is pending, else held for {@link #runTail}. */
    void respond(ChannelHandlerContext ctx, ByteBuf buf, boolean close) {
        if (open || count != 0) {
            hold(ctx, buf, close);
        } else {
            write(ctx, buf, close);
        }
    }

    /**
     * After any failed statement. A constraint failure rolls back only that statement; most other errors inside a
     * transaction make SQLite roll back the whole transaction, which {@code autocommit()} reveals. Then every held
     * response produced inside it becomes 500, and the next write opens a new transaction.
     */
    void onDbError() {
        if (open && db.autocommit()) {
            open = false;
            log.warn("SQLite rolled back the pass transaction; {} held responses become 500", count - txStart);
            failHeld(txStart, count);
        }
    }

    void noteRequest() {
        if (stallNanos > 0 && passRequests++ == 0 && passAccepts == 0) {
            passStartNanos = System.nanoTime();
            schedule();
        }
    }

    void noteAccept() {
        if (stallNanos > 0 && passAccepts++ == 0 && passRequests == 0) {
            passStartNanos = System.nanoTime();
            schedule();
        }
    }

    /** Commits and flushes whatever is pending; used at shutdown. */
    void flushNow() {
        if (open || count != 0) {
            runTail();
        }
    }

    private void schedule() {
        if (!scheduled) {
            scheduled = true;
            loop.executeAfterEventLoopIteration(tail);
        }
    }

    private void hold(ChannelHandlerContext ctx, ByteBuf buf, boolean close) {
        int i = count;
        if (i == bufs.length) {
            int n = i * 2;
            ctxs = Arrays.copyOf(ctxs, n);
            bufs = Arrays.copyOf(bufs, n);
            closes = Arrays.copyOf(closes, n);
        }
        ctxs[i] = ctx;
        bufs[i] = buf;
        closes[i] = close;
        count = i + 1;
        schedule();
    }

    private void failHeld(int from, int to) {
        for (int i = from; i < to; i++) {
            ByteBuf b = bufs[i];
            if (!Responses.isInterim(b)) {
                b.release();
                bufs[i] = Responses.fixed(Responses.INTERNAL_ERROR, closes[i]);
            }
        }
    }

    private void runTail() {
        scheduled = false;
        inTail = true;
        try {
            int n = count;
            long commitNanos = 0;
            if (open) {
                long t0 = System.nanoTime();
                int rc = failCommitsForTest ? Sqlite.SQLITE_ERROR : db.commit();
                commitNanos = System.nanoTime() - t0;
                lastCommitRc = rc;
                if (rc == Sqlite.SQLITE_DONE) {
                    commits++;
                } else {
                    commitFailures++;
                    if (commitErrors.tryAcquire()) {
                        log.warn("COMMIT failed: rc {}, extended {}: {}; {} held responses become 500 "
                                        + "({} similar suppressed)", rc, db.lastExtendedErrcode(),
                                db.lastErrorMessage(), n - txStart, commitErrors.takeSuppressed());
                    }
                    db.rollback();
                    failHeld(txStart, n);
                }
                open = false;
            }
            // Every transaction that produced a response in [0, n) has now committed, or its responses are 500s.
            ChannelHandlerContext[] c = ctxs;
            ByteBuf[] b = bufs;
            boolean[] cl = closes;
            for (int i = 0; i < n; i++) {
                ChannelHandlerContext ctx = c[i];
                ByteBuf buf = b[i];
                c[i] = null;
                b[i] = null;
                write(ctx, buf, cl[i]);
            }
            int rest = count - n;
            if (rest > 0) {
                // Responses produced while flushing (only possible through re-entrant channel events) wait for the
                // tail run that their hold() registered.
                System.arraycopy(c, n, c, 0, rest);
                System.arraycopy(b, n, b, 0, rest);
                System.arraycopy(cl, n, cl, 0, rest);
                Arrays.fill(c, rest, count, null);
                Arrays.fill(b, rest, count, null);
                if (open) {
                    txStart -= n;
                }
            }
            count = rest;
            long ckptNanos = maybeCheckpoint();
            if (stallNanos > 0) {
                long elapsed = System.nanoTime() - passStartNanos;
                if ((passRequests > 0 || passAccepts > 0) && elapsed >= stallNanos) {
                    log.info("slow pass: {} ms, {} requests, {} held, {} accepts, commit {} us, checkpoint {} us",
                            elapsed / 1_000_000, passRequests, n, passAccepts, commitNanos / 1000, ckptNanos / 1000);
                }
                passRequests = 0;
                passAccepts = 0;
            }
        } finally {
            inTail = false;
        }
    }

    /** RESTART checkpoint when the WAL is past the threshold and no transaction is open; returns its duration. */
    private long maybeCheckpoint() {
        if (open || db.walFrames() < ckptFrames || !db.autocommit()) {
            return 0;
        }
        try {
            long ns = db.checkpoint();
            checkpoints++;
            if (ns > checkpointMaxNanos) {
                checkpointMaxNanos = ns;
            }
            return ns;
        } catch (RuntimeException e) {
            // The frame count stays above the threshold, so the next pass retries.
            checkpointFailures++;
            if (checkpointErrors.tryAcquire()) {
                log.warn("checkpoint failed: {} ({} similar suppressed)", e.getMessage(),
                        checkpointErrors.takeSuppressed());
            }
            return 0;
        }
    }

    private static void write(ChannelHandlerContext ctx, ByteBuf buf, boolean close) {
        try {
            if (close) {
                ctx.writeAndFlush(buf).addListener(ChannelFutureListener.CLOSE);
            } else {
                ctx.writeAndFlush(buf, ctx.voidPromise());
            }
        } catch (RuntimeException e) {
            if (buf.refCnt() > 0) {
                buf.release();
            }
            ctx.close();
        }
    }

    // ---- stats and test hooks (event loop only)

    boolean isOpen() {
        return open;
    }

    boolean inTail() {
        return inTail;
    }

    int lastCommitRc() {
        return lastCommitRc;
    }

    long commits() {
        return commits;
    }

    long commitFailures() {
        return commitFailures;
    }

    long checkpoints() {
        return checkpoints;
    }

    long checkpointFailures() {
        return checkpointFailures;
    }

    /** Longest checkpoint since the last call. */
    long takeCheckpointMaxNanos() {
        long m = checkpointMaxNanos;
        checkpointMaxNanos = 0;
        return m;
    }
}
