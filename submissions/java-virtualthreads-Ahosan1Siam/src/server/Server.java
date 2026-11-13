package server;

import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class Server {
    public static void main(String[] args) {
        String host = System.getenv().getOrDefault("HOST", "0.0.0.0");
        int port = Integer.parseInt(System.getenv().getOrDefault("PORT", "3000"));
        String sqlitePath = System.getenv().getOrDefault("SQLITE_PATH", "seed/feed.db");
        String jwtSecret = System.getenv().getOrDefault("JWT_SECRET", "twelve-dollar-challenge");

        long startTimeMs = System.currentTimeMillis();

        try {
            System.out.println("Initializing JWT auth...");
            JwtAuth.init(jwtSecret);

            System.out.println("Initializing SQLite database at: " + sqlitePath);
            Database.init(sqlitePath);

            HttpHandler handler = new HttpHandler(startTimeMs);
            ExecutorService vThreadExecutor = Executors.newVirtualThreadPerTaskExecutor();

            InetAddress bindAddr = InetAddress.getByName(host);
            try (ServerSocket serverSocket = new ServerSocket(port, 8192, bindAddr)) {
                serverSocket.setReuseAddress(true);
                System.out.println("Server listening on " + host + ":" + port + " with Java Virtual Threads.");

                while (!serverSocket.isClosed()) {
                    Socket socket = serverSocket.accept();
                    vThreadExecutor.submit(() -> handler.handleConnection(socket));
                }
            }
        } catch (Exception e) {
            System.err.println("Fatal server error: " + e.getMessage());
            e.printStackTrace();
            System.exit(1);
        }
    }
}

