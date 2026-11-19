package app;

import java.util.ArrayList;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.locks.LockSupport;

/**
 * The single write path. Writes go to a dedicated platform thread so that a COMMIT -- which can
 * trigger a WAL checkpoint and an fsync -- never blocks the carrier thread that serves reads.
 *
 * Whatever has queued up while the previous transaction was committing is committed together
 * (group commit, allowed by rule 6), and every request still waits for its own commit before it
 * answers: a 201 always means the row is on disk.
 */
final class Writer {
    private Writer() {}

    private static final int MAX_BATCH = 256;
    private static final ArrayBlockingQueue<WriteOp> QUEUE = new ArrayBlockingQueue<>(8192);

    static void start() {
        Thread t = new Thread(Writer::loop, "writer");
        t.setDaemon(true);
        t.start();
    }

    /** Submits the op and waits for its commit. Returns false if the write failed. */
    static boolean run(WriteOp op) {
        op.prepare(Thread.currentThread());
        try {
            QUEUE.put(op);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
        while (op.pending()) LockSupport.park(op);
        return op.status == 0;
    }

    private static void loop() {
        ArrayList<WriteOp> batch = new ArrayList<>(MAX_BATCH);
        for (;;) {
            batch.clear();
            try {
                batch.add(QUEUE.take());
            } catch (InterruptedException e) {
                return;
            }
            QUEUE.drainTo(batch, MAX_BATCH - 1);
            try {
                commit(batch);
            } catch (Throwable t) {
                t.printStackTrace();
                for (int i = 0; i < batch.size(); i++) batch.get(i).fail();
            }
            for (int i = 0; i < batch.size(); i++) batch.get(i).complete();
        }
    }

    private static void commit(ArrayList<WriteOp> batch) {
        boolean committed = false;
        if (step(Db.stBegin)) {
            for (int i = 0; i < batch.size(); i++) {
                WriteOp op = batch.get(i);
                try {
                    op.execute();
                } catch (Throwable t) {
                    op.fail();
                    t.printStackTrace();
                }
            }
            committed = step(Db.stCommit);
        }
        if (!committed) {
            step(Db.stRollback);
            for (int i = 0; i < batch.size(); i++) batch.get(i).fail();
        }
    }

    private static boolean step(java.lang.foreign.MemorySegment stmt) {
        int rc = Sqlite.step(stmt);
        Sqlite.reset(stmt);
        if (rc != Sqlite.DONE) {
            System.err.println("writer: rc=" + rc + ": " + Sqlite.errmsg(Db.writeDb));
            return false;
        }
        return true;
    }
}
