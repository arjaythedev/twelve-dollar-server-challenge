package server;

import java.net.Socket;
import java.util.concurrent.ConcurrentLinkedDeque;
import java.util.concurrent.atomic.AtomicInteger;

public final class ConnectionTracker {
    private static final int MAX_CONNECTIONS_THRESHOLD = 60000;
    private static final AtomicInteger ACTIVE_CONNECTIONS = new AtomicInteger(0);
    private static final ConcurrentLinkedDeque<Socket> IDLE_SOCKETS = new ConcurrentLinkedDeque<>();

    private ConnectionTracker() {}

    public static void onConnectionAccepted(Socket socket) {
        int count = ACTIVE_CONNECTIONS.incrementAndGet();
        if (count >= MAX_CONNECTIONS_THRESHOLD) {
            evictOldestIdleConnection();
        }
    }

    public static void markIdle(Socket socket) {
        IDLE_SOCKETS.offerLast(socket);
    }

    public static void markActive(Socket socket) {
        IDLE_SOCKETS.remove(socket);
    }

    public static void onConnectionClosed(Socket socket) {
        ACTIVE_CONNECTIONS.decrementAndGet();
        IDLE_SOCKETS.remove(socket);
    }

    private static void evictOldestIdleConnection() {
        Socket idle = IDLE_SOCKETS.pollFirst();
        if (idle != null && !idle.isClosed()) {
            try {
                // SO_LINGER=0 sends a TCP RST to reset connection immediately without entering TIME_WAIT
                idle.setSoLinger(true, 0);
                idle.close();
            } catch (Exception ignored) {}
        }
    }

    public static int getActiveConnections() {
        return ACTIVE_CONNECTIONS.get();
    }
}

