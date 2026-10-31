package app;

import java.lang.foreign.MemorySegment;
import java.util.concurrent.locks.LockSupport;

/**
 * One pending write, owned by the requesting virtual thread and executed by the writer thread.
 * There is exactly one per {@link Out}, so a write costs no allocation. All the bytes it needs
 * (the trimmed body, the username) live in the Out buffer, which the request holds until the
 * response has been written -- that is why they can be bound to SQLite by pointer.
 */
final class WriteOp {
    static final int CREATE = 1, LIKE = 2;
    private static final int PENDING = 0, FINISHED = 1;

    final MemorySegment seg;

    int kind;
    long userId;
    long postId;          // LIKE
    int bodyLen;          // CREATE: trimmed body staged at Out.INS_OFF
    int nameOff, nameLen; // CREATE: username bytes inside the Out buffer
    int outPos;           // CREATE: where the RETURNING json goes
    int outLen;           // CREATE: how many bytes were written there
    boolean alreadyLiked; // LIKE
    int status;           // 0 = ok, 1 = failed

    Thread waiter;
    volatile int state;

    WriteOp(Out out) { this.seg = out.seg; }

    /** Runs on the writer thread, inside the batch transaction. */
    void execute() {
        status = 1;
        if (kind == CREATE) {
            MemorySegment st = Db.stInsertPost;
            try {
                Sqlite.bindLong(st, 1, userId);
                Sqlite.bindText(st, 2, seg, Out.INS_OFF, bodyLen);
                Sqlite.bindText(st, 3, seg, nameOff, nameLen);
                if (Sqlite.step(st) != Sqlite.ROW) return;
                MemorySegment p = Sqlite.columnText(st, 0);
                int n = Sqlite.columnBytes(st, 0);
                if (n <= 0 || outPos + n > Out.BODY_LIMIT) return;
                MemorySegment.copy(p.reinterpret(n), 0, seg, outPos, n);
                outLen = n;
                Sqlite.step(st);                 // drain the RETURNING cursor to DONE
                status = 0;
            } finally {
                Sqlite.reset(st);
            }
        } else {
            MemorySegment st = Db.stInsertLike;
            try {
                Sqlite.bindLong(st, 1, userId);
                Sqlite.bindLong(st, 2, postId);
                if (Sqlite.step(st) != Sqlite.DONE) return;
                alreadyLiked = Sqlite.changes(Db.writeDb) == 0;
                status = 0;
            } finally {
                Sqlite.reset(st);
            }
        }
    }

    void prepare(Thread owner) {
        status = 0;
        outLen = 0;
        alreadyLiked = false;
        waiter = owner;
        state = PENDING;
    }
    boolean pending() { return state == PENDING; }
    void fail() { status = 1; }
    void complete() {
        state = FINISHED;                 // volatile write publishes every field set above
        LockSupport.unpark(waiter);
    }
}
