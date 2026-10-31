package feed.http;

/**
 * Tuning flags, read from system properties so experiments need no code change. None of them is read from the
 * environment: start.sh passes no flags, so the benchmark box always runs the defaults.
 *
 * @param ckptFrames     {@code feed.ckptFrames} (1000): RESTART checkpoint once the WAL has this many frames
 * @param stallLogMs     {@code feed.stallLogMs} (0 = off): log event-loop passes slower than this
 * @param maxOpen        {@code feed.maxOpen} (65000): above this many open connections, responses close the connection;
 *                       the effective limit is lower when the descriptor limit requires it ({@link Connections#limitFor})
 * @param idleTimeoutSec {@code feed.idleTimeoutSec} (120): close connections without a new request for this long
 * @param sweepSec       {@code feed.sweepSec} (5): idle sweep interval
 */
public record FeedConfig(int ckptFrames, long stallLogMs, int maxOpen, int idleTimeoutSec, int sweepSec) {

    public static final FeedConfig DEFAULTS = new FeedConfig(1000, 0, 65000, 120, 5);

    public static FeedConfig fromSystemProperties() {
        FeedConfig d = DEFAULTS;
        return new FeedConfig(
                positive("feed.ckptFrames", d.ckptFrames),
                Long.getLong("feed.stallLogMs", d.stallLogMs),
                positive("feed.maxOpen", d.maxOpen),
                positive("feed.idleTimeoutSec", d.idleTimeoutSec),
                positive("feed.sweepSec", d.sweepSec));
    }

    private static int positive(String name, int def) {
        int v = Integer.getInteger(name, def);
        if (v < 1) {
            throw new IllegalStateException(name + " must be positive, got " + v);
        }
        return v;
    }

    @Override
    public String toString() {
        return "ckptFrames=" + ckptFrames + ", maxOpen=" + maxOpen + ", idleTimeoutSec=" + idleTimeoutSec
                + (stallLogMs > 0 ? ", stallLogMs=" + stallLogMs : "");
    }
}
