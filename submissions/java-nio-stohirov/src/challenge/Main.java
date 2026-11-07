package challenge;

import java.lang.management.ManagementFactory;

public final class Main {
    private Main() {}

    public static void main(String[] args) throws Exception {
        String path = required("SQLITE_PATH");
        String secret = required("JWT_SECRET");
        String host = System.getenv().getOrDefault("HOST", "0.0.0.0");
        int port = Integer.parseInt(System.getenv().getOrDefault("PORT", "3000"));

        Db db = new Db(path);
        Server server = new Server(db, new Jwt(secret), host, port, ringSlots());
        System.err.println("listening on " + host + ":" + port);
        server.run();
    }

    private static String required(String name) {
        String v = System.getenv(name);
        if (v == null || v.isEmpty()) throw new IllegalStateException(name + " is not set");
        return v;
    }

    /**
     * A ring's table of direct descriptors can be no larger than RLIMIT_NOFILE. The JVM has already raised
     * its soft limit to the hard one (-XX:+MaxFDLimit). Direct descriptors don't use up that limit.
     */
    private static int ringSlots() {
        long limit = 65535;
        try {
            if (ManagementFactory.getOperatingSystemMXBean() instanceof com.sun.management.UnixOperatingSystemMXBean os) {
                limit = os.getMaxFileDescriptorCount();
            }
        } catch (RuntimeException | LinkageError ignored) {
        }
        return (int) Math.clamp(limit, 1024L, 65535L);
    }
}
