package challenge;

import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import io.netty.channel.EventLoop;

import java.util.ArrayList;

/**
 * State shared by every connection on the (single) event loop thread: the database, the JWT
 * verifier, scratch buffers, and group commit.
 *
 * Group commit: the first write in an event loop iteration opens a transaction and schedules
 * {@link #commitAll} to run after this iteration's I/O. Every write until then joins that
 * transaction, and its connection holds back all responses until the commit is done, so a 201 is
 * only ever sent for a committed row (rule 6).
 */
final class App {
    final Db db;
    final Jwt jwt;
    final long startNanos = System.nanoTime();
    /** A response body under construction. */
    final ByteBuf body = Unpooled.buffer(16 * 1024);
    /** Request bodies are copied here to be parsed. */
    final byte[] request = new byte[64 * 1024];

    private final EventLoop loop;
    private final ArrayList<HttpHandler> awaitingCommit = new ArrayList<>();
    private final Runnable commitTask = this::commitAll;

    App(Db db, Jwt jwt, EventLoop loop) {
        this.db = db;
        this.jwt = jwt;
        this.loop = loop;
    }

    /** Call before a write: makes sure a transaction is open and h's responses wait for its commit. */
    void joinWrite(HttpHandler h) {
        if (!db.inTransaction()) {
            db.begin();
            loop.execute(commitTask);
        }
        if (!h.awaitingCommit) {
            h.awaitingCommit = true;
            awaitingCommit.add(h);
        }
    }

    private void commitAll() {
        boolean ok = true;
        try {
            db.commit();
        } catch (RuntimeException e) {
            ok = false;
            System.err.println("commit failed: " + e.getMessage());
        }
        for (int i = 0, n = awaitingCommit.size(); i < n; i++) awaitingCommit.get(i).committed(ok);
        awaitingCommit.clear();
    }
}
