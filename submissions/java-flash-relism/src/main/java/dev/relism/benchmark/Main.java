package dev.relism.benchmark;

import dev.relism.flash.App;
import dev.relism.flash.Server;
import dev.relism.flash.ServerConfig;

public final class Main {
    private Main() {}

    public static void main(String[] args) throws Exception {
        var config = ServerConfig.builder()
                .host(required("HOST")).port(Integer.parseInt(required("PORT")))
                .dev(false).maxConnections(30_000).maxHeaderBufferSize(4096)
                .maxBodySize(16 * 1024).idleKeepAliveTimeoutMs(75_000).build();
        var app = App.create().install(new FeedApp(required("SQLITE_PATH"), required("JWT_SECRET")));
        var server = Server.start(app, config);
        Runtime.getRuntime().addShutdownHook(new Thread(() -> server.stop().join(), "shutdown"));
        server.await();
    }

    private static String required(String name) {
        String value = System.getenv(name);
        if (value == null || value.isBlank()) throw new IllegalArgumentException(name + " is required");
        return value;
    }
}
