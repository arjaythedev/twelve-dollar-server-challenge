package server;

import io.netty.bootstrap.ServerBootstrap;
import io.netty.buffer.PooledByteBufAllocator;
import io.netty.channel.Channel;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.ChannelOption;
import io.netty.channel.ChannelPipeline;
import io.netty.channel.EventLoopGroup;
import io.netty.channel.ServerChannel;
import io.netty.channel.epoll.Epoll;
import io.netty.channel.epoll.EpollChannelOption;
import io.netty.channel.epoll.EpollEventLoopGroup;
import io.netty.channel.epoll.EpollServerSocketChannel;
import io.netty.channel.nio.NioEventLoopGroup;
import io.netty.channel.socket.SocketChannel;
import io.netty.channel.socket.nio.NioServerSocketChannel;
import io.netty.handler.codec.http.HttpObjectAggregator;
import io.netty.handler.codec.http.HttpServerCodec;
import server.db.Database;
import server.util.FastJwt;

import java.util.concurrent.TimeUnit;

public final class HttpServer {

    public static void main(String[] args) throws Exception {
        String sqlitePath = System.getenv("SQLITE_PATH");
        if (sqlitePath == null || sqlitePath.isEmpty()) {
            sqlitePath = "feed.db";
        }

        String jwtSecret = System.getenv("JWT_SECRET");
        if (jwtSecret == null || jwtSecret.isEmpty()) {
            jwtSecret = "twelve-dollar-challenge";
        }

        String host = System.getenv("HOST");
        if (host == null || host.isEmpty()) {
            host = "127.0.0.1";
        }

        int port = 3000;
        String portStr = System.getenv("PORT");
        if (portStr != null && !portStr.isEmpty()) {
            try {
                port = Integer.parseInt(portStr);
            } catch (NumberFormatException ignored) {}
        }

        Database db = new Database(sqlitePath);
        FastJwt jwt = new FastJwt(jwtSecret);
        long startTime = System.currentTimeMillis();

        EventLoopGroup group;
        Class<? extends ServerChannel> channelClass;

        if (Epoll.isAvailable()) {
            group = new EpollEventLoopGroup(1);
            channelClass = EpollServerSocketChannel.class;
        } else {
            group = new NioEventLoopGroup(1);
            channelClass = NioServerSocketChannel.class;
        }

        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            try {
                group.shutdownGracefully(0, 1, TimeUnit.SECONDS).sync();
                db.close();
            } catch (Exception ignored) {}
        }));

        try {
            ServerBootstrap b = new ServerBootstrap();
            b.group(group)
             .channel(channelClass)
             .option(ChannelOption.SO_BACKLOG, 8192)
             .childOption(ChannelOption.ALLOCATOR, PooledByteBufAllocator.DEFAULT)
             .childOption(ChannelOption.TCP_NODELAY, true)
             .childOption(ChannelOption.SO_KEEPALIVE, true)
             .childOption(ChannelOption.SO_RCVBUF, 4096)
             .childOption(ChannelOption.SO_SNDBUF, 16384);

            if (Epoll.isAvailable()) {
                b.option(EpollChannelOption.SO_REUSEPORT, true);
                b.childOption(EpollChannelOption.TCP_QUICKACK, true);
            }

            RequestHandler requestHandler = new RequestHandler(db, jwt, startTime);

            b.childHandler(new ChannelInitializer<SocketChannel>() {
                @Override
                protected void initChannel(SocketChannel ch) {
                    ChannelPipeline p = ch.pipeline();
                    p.addLast(new HttpServerCodec(1024, 2048, 8192));
                    p.addLast(new HttpObjectAggregator(16384));
                    p.addLast(requestHandler);
                }
            });

            Channel ch = b.bind(host, port).sync().channel();
            System.out.println("Java Netty server listening on " + host + ":" + port + " (epoll=" + Epoll.isAvailable() + ")");
            ch.closeFuture().sync();
        } finally {
            group.shutdownGracefully(0, 1, TimeUnit.SECONDS);
            db.close();
        }
    }
}
