package feed;

import org.springframework.core.env.Environment;

/** Runtime configuration from the SPEC environment variables. */
public record FeedProperties(String sqlitePath, String jwtSecret, String host, int port) {

    public static FeedProperties fromEnvironment(Environment env) {
        String sqlitePath = required(env, "SQLITE_PATH");
        String jwtSecret = required(env, "JWT_SECRET");
        String host = env.getProperty("HOST", "0.0.0.0");
        String portText = env.getProperty("PORT", "3000");
        int port;
        try {
            port = Integer.parseInt(portText.trim());
        } catch (NumberFormatException e) {
            throw new IllegalStateException("PORT is not a number: " + portText);
        }
        if (port < 0 || port > 65535) {
            throw new IllegalStateException("PORT out of range: " + port);
        }
        return new FeedProperties(sqlitePath, jwtSecret, host, port);
    }

    private static String required(Environment env, String name) {
        String value = env.getProperty(name);
        if (value == null || value.isEmpty()) {
            throw new IllegalStateException(name + " is required");
        }
        return value;
    }

    @Override
    public String toString() {
        return "FeedProperties[sqlitePath=" + sqlitePath + ", host=" + host + ", port=" + port + "]";
    }
}
