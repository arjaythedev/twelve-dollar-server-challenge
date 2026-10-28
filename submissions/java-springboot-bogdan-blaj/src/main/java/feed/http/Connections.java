package feed.http;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.lang.management.ManagementFactory;
import java.lang.management.OperatingSystemMXBean;

/**
 * Open connections: a count and an intrusive LRU list of the handlers (most recently active first).
 *
 * <p>A connection becomes active when it opens and whenever a request head arrives. A sweep every
 * {@code sweepSec} walks from the least recently active end and closes connections inactive for
 * {@code idleTimeoutSec}; that covers both idle keep-alive connections and connections stuck mid-request. The
 * clock is coarse (seconds since start, updated by the sweep), so the timeout is exact to within one sweep interval.
 *
 * <p>Near the descriptor limit ({@link #overLimit()}) responses carry {@code Connection: close} and the channel
 * closes after the flush, so accept() keeps finding free descriptors. The threshold is
 * {@code min(feed.maxOpen, maxFd - reserve)} ({@link #limitFor}): the process's own descriptors (about 15 after
 * startup: jar, jimage, database and WAL, epoll, eventfd, timerfd, pipes, listener, plus one per jcmd attach) must
 * fit in the reserve, otherwise keep-alive connections could take every descriptor and accept() would fail until the
 * idle sweep frees some.
 *
 * <p>Event loop only.
 */
final class Connections {

    private static final Logger log = LoggerFactory.getLogger(Connections.class);
    private static final int STATS_EVERY_SEC = 60;

    private final int maxOpen;
    private final int limit;
    private final long maxFd;
    private final int idleTimeoutSec;
    private final Batch batch;
    private final long baseNanos = System.nanoTime();

    private FeedHandler head;
    private FeedHandler tail;
    private int open;
    private int peak;
    private int nowSec;
    private int lastStatsSec;
    private boolean overLimitLogged;

    private long accepted;
    private long idleClosed;
    private long limitCloses;
    private long loggedCommits = -1;
    private long loggedCheckpoints = -1;

    Connections(FeedConfig config, Batch batch) {
        this.maxOpen = config.maxOpen();
        this.maxFd = maxFileDescriptors();
        this.limit = limitFor(maxOpen, maxFd);
        this.idleTimeoutSec = config.idleTimeoutSec();
        this.batch = batch;
    }

    void add(FeedHandler h) {
        if (h.counted) {
            return;
        }
        h.counted = true;
        accepted++;
        int n = ++open;
        if (n > peak) {
            peak = n;
        }
        h.lastActiveSec = nowSec;
        linkFirst(h);
        batch.noteAccept();
        if (n > limit && !overLimitLogged) {
            overLimitLogged = true;
            log.warn("{} open connections, above the limit of {}: responses now close their connection", n, limit);
        }
    }

    void remove(FeedHandler h) {
        if (h.linked) {
            unlink(h);
        }
        if (h.counted) {
            h.counted = false;
            open--;
            // Hysteresis: at the limit, every guarded response closes one connection and the next accept opens one,
            // so re-arming at the limit itself would log two lines per connection.
            if (overLimitLogged && open <= limit - Math.max(1, limit / 16)) {
                overLimitLogged = false;
                log.info("{} open connections, back below the limit of {} ({} limit-closed so far)", open, limit,
                        limitCloses);
            }
        }
    }

    void touch(FeedHandler h) {
        h.lastActiveSec = nowSec;
        if (h.linked && head != h) {
            unlink(h);
            linkFirst(h);
        }
    }

    boolean overLimit() {
        return open > limit;
    }

    String describeLimit() {
        return "connection limit " + limit + " (feed.maxOpen=" + maxOpen + ", fd limit "
                + (maxFd > 0 ? maxFd + ", reserve " + reserveFor(maxFd) : "unknown") + ")";
    }

    /**
     * {@code min(maxOpen, maxFd - reserveFor(maxFd))}, at least 1; just {@code maxOpen} when the descriptor limit is
     * unknown ({@code maxFd <= 0}).
     */
    static int limitFor(int maxOpen, long maxFd) {
        if (maxFd <= 0) {
            return maxOpen;
        }
        return (int) Math.max(1, Math.min(maxOpen, maxFd - reserveFor(maxFd)));
    }

    /** Descriptors kept free for the process itself: maxFd / 8, clamped to [64, 512]. */
    static long reserveFor(long maxFd) {
        return Math.max(64, Math.min(512, maxFd / 8));
    }

    /** The soft RLIMIT_NOFILE of this process, or -1 when the platform does not report it. */
    static long maxFileDescriptors() {
        OperatingSystemMXBean os = ManagementFactory.getOperatingSystemMXBean();
        if (os instanceof com.sun.management.UnixOperatingSystemMXBean unix) {
            return unix.getMaxFileDescriptorCount();
        }
        return -1;
    }

    void countLimitClose() {
        limitCloses++;
    }

    int open() {
        return open;
    }

    /** Periodic task on the loop: advance the clock, close inactive connections, log stats once a minute. */
    void sweep() {
        nowSec = (int) ((System.nanoTime() - baseNanos) / 1_000_000_000L);
        FeedHandler h = tail;
        int closed = 0;
        while (h != null && nowSec - h.lastActiveSec >= idleTimeoutSec) {
            FeedHandler prev = h.prev;
            unlink(h);
            h.closeInactive();
            closed++;
            h = prev;
        }
        idleClosed += closed;
        if (nowSec - lastStatsSec >= STATS_EVERY_SEC) {
            lastStatsSec = nowSec;
            logStats();
        }
    }

    private void logStats() {
        long commits = batch.commits();
        long checkpoints = batch.checkpoints();
        if (commits == loggedCommits && checkpoints == loggedCheckpoints && open == 0) {
            return;
        }
        loggedCommits = commits;
        loggedCheckpoints = checkpoints;
        log.info("stats: open {} (peak {}), accepted {}, idle-closed {}, limit-closed {}, commits {} (failed {}), "
                        + "checkpoints {} (failed {}, max {} ms in last {} s)", open, peak, accepted, idleClosed,
                limitCloses, commits, batch.commitFailures(), checkpoints, batch.checkpointFailures(),
                batch.takeCheckpointMaxNanos() / 1_000_000, STATS_EVERY_SEC);
    }

    private void linkFirst(FeedHandler h) {
        h.prev = null;
        h.next = head;
        if (head != null) {
            head.prev = h;
        } else {
            tail = h;
        }
        head = h;
        h.linked = true;
    }

    private void unlink(FeedHandler h) {
        FeedHandler p = h.prev;
        FeedHandler n = h.next;
        if (p != null) {
            p.next = n;
        } else {
            head = n;
        }
        if (n != null) {
            n.prev = p;
        } else {
            tail = p;
        }
        h.prev = null;
        h.next = null;
        h.linked = false;
    }
}
