package challenge;

import java.util.concurrent.locks.LockSupport;

/**
 * Runs WAL checkpoints on their own thread and connection, at the lowest priority, so the event loop
 * never waits for their fsyncs. A PASSIVE checkpoint never waits on the loop's locks, and the loop never
 * waits on this thread.
 *
 * Under steady writes a checkpoint can't copy the frames written while it ran, and the log only restarts
 * once it is fully copied. So after the bulk pass, writes are held (reads go on) while this thread copies
 * the tail, for at most {@link #HOLD_MAX_NANOS}. The next write then restarts the log.
 */
final class Checkpointer implements Runnable {
    private static final int IDLE = 0, BULK = 1, BULK_DONE = 2, TAIL = 3, TAIL_DONE = 4;
    private static final int START_FRAMES = 4096, FORCE_FRAMES = 65536;
    private static final long HOLD_MAX_NANOS = 250_000_000L;

    private final long connection;
    private final Thread thread;
    private volatile int state = IDLE;
    private volatile boolean copiedAll;
    private boolean held;
    private long heldSince;

    Checkpointer(long connection) {
        this.connection = connection;
        thread = new Thread(this, "checkpoint");
        thread.setDaemon(true);
        thread.start();
    }

    @Override
    public void run() {
        Sys.lowestPriority();
        while (true) {
            int s = state;
            if (s != BULK && s != TAIL) {
                LockSupport.park(this);
                continue;
            }
            copiedAll = Sqlite.walCheckpointPassive(connection);
            state = s + 1;
        }
    }

    private void start(int next) {
        state = next;
        LockSupport.unpark(thread);
    }

    boolean writesHeld() { return held; }

    /** Between passes, with no transaction open. A hold starts and ends only here. */
    void tick(long mainConnection) {
        switch (state) {
            case IDLE -> {
                int frames = Sqlite.walFrames();
                if (frames >= FORCE_FRAMES) {
                    // Last resort: the thread can't get CPU. It is idle, so this copies the whole log.
                    if (Sqlite.walCheckpointPassive(mainConnection)) Sqlite.walFramesReset();
                } else if (frames >= START_FRAMES) {
                    start(BULK);
                }
            }
            case BULK_DONE -> {
                held = true;
                heldSince = System.nanoTime();
                start(TAIL);
            }
            case TAIL_DONE -> {
                if (copiedAll) Sqlite.walFramesReset();
                held = false;
                state = IDLE;
            }
            default -> {
                if (held && System.nanoTime() - heldSince > HOLD_MAX_NANOS) held = false;
            }
        }
    }
}
