package app;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.StandardSocketOptions;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;

/** Reads the env, opens the database, starts the writer thread, then accepts forever. */
public final class Main {
    static final long START_NANOS = System.nanoTime();
    static byte[] jwtSecret;

    public static void main(String[] args) throws IOException {
        String dbPath = System.getenv("SQLITE_PATH");
        if (dbPath == null || dbPath.isEmpty()) {
            System.err.println("SQLITE_PATH is required");
            System.exit(2);
        }
        String secret = System.getenv("JWT_SECRET");
        jwtSecret = (secret == null ? "" : secret).getBytes(StandardCharsets.UTF_8);
        String host = env("HOST", "0.0.0.0");
        int port = Integer.parseInt(env("PORT", "80"));

        Db.open(dbPath);
        Writer.start();

        ServerSocketChannel server = ServerSocketChannel.open();
        server.setOption(StandardSocketOptions.SO_REUSEADDR, true);
        server.bind(new InetSocketAddress(host, port), 4096);
        System.err.println("listening on " + host + ":" + port);

        for (;;) {
            SocketChannel ch;
            try {
                ch = server.accept();
            } catch (IOException e) {
                continue;                       // out of fds, or the peer vanished mid-handshake
            }
            try {
                ch.setOption(StandardSocketOptions.TCP_NODELAY, true);
            } catch (IOException ignored) {
                // not fatal
            }
            Thread.startVirtualThread(new Connection(ch));
        }
    }

    private static String env(String name, String fallback) {
        String v = System.getenv(name);
        return v == null || v.isEmpty() ? fallback : v;
    }
}
