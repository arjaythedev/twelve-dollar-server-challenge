package app;

import java.io.IOException;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.channels.SocketChannel;

/**
 * One virtual thread per connection, blocking I/O. Idle keep-alive connections cost a parked
 * virtual thread plus this 1 KB read buffer, which is what makes ~15k direct connections
 * affordable; the 80 KB render buffer is borrowed only while a request is being served.
 *
 * There is no idle timeout: the spec wants connections held for at least 65 s, and Nginx (if it is
 * in front) must never find a socket we closed under it.
 */
final class Connection implements Runnable {
    private static final int READ_BUF = 1024;
    private static final int MAX_BUF = 32768;      // bodies past 16 KB are out of scope

    private final SocketChannel ch;
    private final Http.Req req = new Http.Req();

    Connection(SocketChannel ch) { this.ch = ch; }

    @Override
    public void run() {
        try (SocketChannel c = ch) {
            buffer(ByteBuffer.allocateDirect(READ_BUF));
            serve(c);
        } catch (IOException e) {
            // the client went away: nothing to say about it
        } catch (Throwable t) {
            t.printStackTrace();
        }
    }

    private void buffer(ByteBuffer b) {
        req.buf = b;
        req.seg = MemorySegment.ofBuffer(b);
    }

    private void serve(SocketChannel c) throws IOException {
        int start = 0;
        for (;;) {
            int headEnd;
            while ((headEnd = Http.findHeadEnd(req.buf, start, req.buf.position())) < 0) {
                if (req.buf.position() == req.buf.capacity()) {
                    if (start > 0) { compact(start); start = 0; continue; }
                    if (!grow()) { fail(c); return; }
                }
                if (c.read(req.buf) < 0) return;
            }
            if (!Http.parse(req, start, headEnd)) { fail(c); return; }

            int len = req.contentLength;
            if (headEnd + len > MAX_BUF) { fail(c); return; }
            if (len > 0 && req.expectContinue && req.buf.position() < headEnd + len) {
                writeAll(c, ByteBuffer.wrap(Responses.CONTINUE));
            }
            while (req.buf.position() - headEnd < len) {
                if (headEnd + len > req.buf.capacity() && !grow()) { fail(c); return; }
                if (c.read(req.buf) < 0) return;
            }
            req.bodyOff = headEnd;

            handle(c);
            if (!req.keepAlive) return;

            start = headEnd + len;
            if (start == req.buf.position()) {
                if (req.buf.capacity() > READ_BUF) buffer(ByteBuffer.allocateDirect(READ_BUF));
                else req.buf.clear();
                start = 0;
            }
        }
    }

    private void handle(SocketChannel c) throws IOException {
        Out out = Out.acquire();
        try {
            out.reset();
            int status;
            try {
                status = Router.dispatch(req, out);
            } catch (Throwable t) {
                t.printStackTrace();
                status = Handlers.error(out, Responses.ERR_INTERNAL, Responses.S500);
            }
            out.finish(status, req.keepAlive, req.http10);
            writeAll(c, out.bb);
        } finally {
            Out.release(out);
        }
    }

    private static void writeAll(SocketChannel c, ByteBuffer b) throws IOException {
        while (b.hasRemaining()) c.write(b);
    }

    /** Last resort for a request we will not parse: say so once, then close. */
    private void fail(SocketChannel c) throws IOException {
        Out out = Out.acquire();
        try {
            out.reset();
            out.put(Responses.ERR_BAD_REQUEST);
            out.finish(Responses.S400, false, false);
            writeAll(c, out.bb);
        } finally {
            Out.release(out);
        }
    }

    /** Keeps offsets stable: the data stays at the same indices, the buffer just gets bigger. */
    private boolean grow() {
        ByteBuffer old = req.buf;
        if (old.capacity() >= MAX_BUF) return false;
        ByteBuffer bigger = ByteBuffer.allocateDirect(Math.min(old.capacity() * 4, MAX_BUF));
        int filled = old.position();
        old.flip();
        bigger.put(old);
        bigger.position(filled);
        buffer(bigger);
        return true;
    }

    private void compact(int start) {
        ByteBuffer b = req.buf;
        b.limit(b.position()).position(start);
        b.compact();
    }
}
