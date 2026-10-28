package feed.http;

/** Lets one message through per interval and counts the ones it held back. Single-threaded. */
final class RateLimit {

    private final long intervalNanos;
    private long next = Long.MIN_VALUE;
    private int suppressed;

    RateLimit(long intervalMillis) {
        this.intervalNanos = intervalMillis * 1_000_000L;
    }

    /** True if the caller should log now; then {@link #takeSuppressed()} tells how many were skipped before. */
    boolean tryAcquire() {
        long now = System.nanoTime();
        if (next != Long.MIN_VALUE && now - next < 0) {
            suppressed++;
            return false;
        }
        next = now + intervalNanos;
        return true;
    }

    int takeSuppressed() {
        int s = suppressed;
        suppressed = 0;
        return s;
    }
}
