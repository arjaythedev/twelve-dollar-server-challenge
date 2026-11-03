package challenge;

import java.io.IOException;

public final class Main {
    private Main() {}

    public static void main(String[] args) throws Exception {
        String path = required("SQLITE_PATH");
        String secret = required("JWT_SECRET");
        String host = System.getenv().getOrDefault("HOST", "0.0.0.0");
        int port = Integer.parseInt(System.getenv().getOrDefault("PORT", "3000"));
        Db db = new Db(path);
        App app = new App(db, new Jwt(secret));
        boolean linux = System.getProperty("os.name", "").toLowerCase().contains("linux");
        if (linux) {
            try {
                System.err.println("listening on " + host + ":" + port + " (io_uring)");
                UringServer.run(app, db, host, port);
                return;
            } catch (IOException e) {
                System.err.println("io_uring setup failed (" + e.getMessage() + "); using java.nio");
            }
        }
        System.err.println("listening on " + host + ":" + port + " (java.nio)");
        new NioServer(app, db).run(host, port);
    }

    private static String required(String name) {
        String value = System.getenv(name);
        if (value == null || value.isEmpty()) throw new IllegalStateException(name + " is not set");
        return value;
    }
}
