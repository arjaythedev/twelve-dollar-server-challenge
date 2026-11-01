package challenge;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.StandardSocketOptions;
import java.nio.ByteBuffer;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Iterator;

/** Single-threaded java.nio loop. Used on macOS and when io_uring cannot be created. */
final class NioServer {
    private final App app;
    private final Db db;

    NioServer(App app, Db db) {
        this.app = app;
        this.db = db;
    }

    void run(String host, int port) throws IOException {
        try (Selector selector = Selector.open(); ServerSocketChannel server = ServerSocketChannel.open()) {
            server.configureBlocking(false);
            server.setOption(StandardSocketOptions.SO_REUSEADDR, true);
            server.bind(new InetSocketAddress(host, port), 4096);
            server.register(selector, SelectionKey.OP_ACCEPT);
            ArrayList<Conn> ready = new ArrayList<>();
            ByteBuffer scratch = ByteBuffer.allocate(16 * 1024);
            while (true) {
                selector.select();
                Iterator<SelectionKey> keys = selector.selectedKeys().iterator();
                while (keys.hasNext()) {
                    SelectionKey key = keys.next();
                    keys.remove();
                    if (!key.isValid()) continue;
                    try {
                        if (key.isAcceptable()) accept(selector, server);
                        if (key.isValid() && key.isReadable()) read((Conn) key.attachment(), scratch, ready);
                        if (key.isValid() && key.isWritable()) write((Conn) key.attachment());
                    } catch (IOException e) {
                        close(key);
                    }
                }
                flush(ready);
                db.checkpointIfDue();
            }
        }
    }

    private void accept(Selector selector, ServerSocketChannel server) throws IOException {
        SocketChannel ch = server.accept();
        while (ch != null) {
            ch.configureBlocking(false);
            ch.setOption(StandardSocketOptions.TCP_NODELAY, true);
            SelectionKey key = ch.register(selector, SelectionKey.OP_READ);
            key.attach(new Conn(key, ch));
            ch = server.accept();
        }
    }

    private void read(Conn conn, ByteBuffer scratch, ArrayList<Conn> ready) throws IOException {
        scratch.clear();
        int n = conn.ch.read(scratch);
        if (n < 0) {
            close(conn.key);
            return;
        }
        if (n == 0) return;
        scratch.flip();
        conn.append(scratch);
        App.Req req = new App.Req();
        while (conn.inLen > 0) {
            int consumed = app.parse(conn.in, conn.inLen, req);
            if (consumed == App.INCOMPLETE) break;
            conn.consume(consumed);
            conn.pending.add(req);
            req = new App.Req();
            if (!ready.contains(conn)) ready.add(conn);
        }
    }

    private void flush(ArrayList<Conn> ready) {
        if (ready.isEmpty()) return;
        db.begin();
        boolean ok = true;
        try {
            for (Conn conn : ready) {
                for (App.Req req : conn.pending) {
                    byte[] bytes = req.early != null ? req.early : app.execute(req);
                    conn.queue(bytes, req.close || req.early != null && req.close);
                }
                conn.pending.clear();
            }
        } catch (RuntimeException e) {
            System.err.println("batch failed: " + e.getMessage());
            ok = false;
            db.rollbackQuietly();
        }
        if (ok && !db.commit()) ok = false;
        if (!ok) {
            byte[] err = app.internalError(true);
            for (Conn conn : ready) {
                conn.out.clear();
                conn.outOff = 0;
                conn.queue(err, true);
            }
        }
        for (Conn conn : ready) interest(conn);
        ready.clear();
    }

    private void write(Conn conn) throws IOException {
        while (!conn.out.isEmpty()) {
            byte[] cur = conn.out.peek();
            int n = conn.ch.write(ByteBuffer.wrap(cur, conn.outOff, cur.length - conn.outOff));
            if (n == 0) break;
            conn.outOff += n;
            if (conn.outOff == cur.length) {
                conn.out.poll();
                conn.outOff = 0;
            }
        }
        if (conn.out.isEmpty() && conn.closeAfter) {
            close(conn.key);
            return;
        }
        interest(conn);
    }

    private static void interest(Conn conn) {
        if (!conn.key.isValid()) return;
        int ops = SelectionKey.OP_READ;
        if (!conn.out.isEmpty()) ops |= SelectionKey.OP_WRITE;
        conn.key.interestOps(ops);
    }

    private static void close(SelectionKey key) {
        try { key.channel().close(); } catch (IOException ignored) { }
        key.cancel();
    }

    private static final class Conn {
        final SelectionKey key;
        final SocketChannel ch;
        byte[] in = new byte[4096];
        int inLen;
        final ArrayDeque<App.Req> pending = new ArrayDeque<>();
        final ArrayDeque<byte[]> out = new ArrayDeque<>();
        int outOff;
        boolean closeAfter;

        Conn(SelectionKey key, SocketChannel ch) {
            this.key = key;
            this.ch = ch;
        }

        void append(ByteBuffer src) {
            int n = src.remaining();
            if (inLen + n > in.length) {
                byte[] next = new byte[Math.max(in.length * 2, inLen + n)];
                System.arraycopy(in, 0, next, 0, inLen);
                in = next;
            }
            src.get(in, inLen, n);
            inLen += n;
        }

        void consume(int n) {
            System.arraycopy(in, n, in, 0, inLen - n);
            inLen -= n;
        }

        void queue(byte[] bytes, boolean close) {
            out.add(bytes);
            if (close) closeAfter = true;
        }
    }
}
