package challenge;

import io.netty.bootstrap.ServerBootstrap;
import io.netty.channel.Channel;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.ChannelOption;
import io.netty.channel.EventLoopGroup;
import io.netty.channel.IoHandlerFactory;
import io.netty.channel.MultiThreadIoEventLoopGroup;
import io.netty.channel.ServerChannel;
import io.netty.channel.epoll.Epoll;
import io.netty.channel.epoll.EpollIoHandler;
import io.netty.channel.epoll.EpollServerSocketChannel;
import io.netty.channel.nio.NioIoHandler;
import io.netty.channel.socket.nio.NioServerSocketChannel;

import java.lang.foreign.MemorySegment;

/**
 * One event loop thread does everything: accept, parse, verify the JWT, run SQLite, write the
 * response. On one vCPU, handing work to other threads would only add context switches. A second
 * thread runs WAL checkpoints so their fsyncs never stall the event loop.
 */
public final class Server {
    public static void main(String[] args) throws Exception {
        String path = env("SQLITE_PATH", null);
        if (path == null) throw new IllegalArgumentException("SQLITE_PATH is not set");
        String secret = env("JWT_SECRET", "twelve-dollar-challenge");
        String host = env("HOST", "127.0.0.1");
        int port = Integer.parseInt(env("PORT", "3000"));

        boolean epoll = Epoll.isAvailable();
        if (!epoll) System.err.println("epoll unavailable, using NIO: " + Epoll.unavailabilityCause());
        IoHandlerFactory io = epoll ? EpollIoHandler.newFactory() : NioIoHandler.newFactory();
        Class<? extends ServerChannel> channel = epoll ? EpollServerSocketChannel.class : NioServerSocketChannel.class;
        EventLoopGroup group = new MultiThreadIoEventLoopGroup(1, io);

        App app = new App(new Db(path), new Jwt(secret), group.next());
        startCheckpointer(path);

        Channel server = new ServerBootstrap()
                .group(group)
                .channel(channel)
                .option(ChannelOption.SO_BACKLOG, 8192)
                .option(ChannelOption.SO_REUSEADDR, true)
                .childOption(ChannelOption.TCP_NODELAY, true)
                .childHandler(new ChannelInitializer<>() {
                    @Override
                    protected void initChannel(Channel ch) {
                        ch.pipeline().addLast(new HttpHandler(app));
                    }
                })
                .bind(host, port).sync().channel();
        System.out.println("listening on " + host + ":" + port + (epoll ? " (epoll)" : " (nio)"));
        server.closeFuture().sync();
    }

    /** PASSIVE checkpoints from their own connection, once a second. */
    private static void startCheckpointer(String path) {
        MemorySegment db = Sqlite.open(path);
        Sqlite.exec(db, "PRAGMA busy_timeout = 1000");
        Thread.ofPlatform().daemon().name("checkpoint").start(() -> {
            while (true) {
                try {
                    Thread.sleep(1000);
                    Sqlite.exec(db, "PRAGMA wal_checkpoint(PASSIVE)");
                } catch (InterruptedException e) {
                    return;
                } catch (RuntimeException e) {
                    System.err.println("checkpoint failed: " + e.getMessage());
                }
            }
        });
    }

    private static String env(String name, String fallback) {
        String v = System.getenv(name);
        return v == null || v.isEmpty() ? fallback : v;
    }
}
