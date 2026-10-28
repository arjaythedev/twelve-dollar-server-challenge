package feed.http;

import feed.FeedProperties;
import feed.db.FeedDb;
import io.netty.bootstrap.ServerBootstrap;
import io.netty.buffer.PooledByteBufAllocator;
import io.netty.channel.Channel;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.ChannelOption;
import io.netty.channel.ChannelPipeline;
import io.netty.channel.EventLoop;
import io.netty.channel.MultiThreadIoEventLoopGroup;
import io.netty.channel.epoll.Epoll;
import io.netty.channel.epoll.EpollIoHandler;
import io.netty.channel.epoll.EpollServerSocketChannel;
import io.netty.handler.codec.http.DefaultHttpHeadersFactory;
import io.netty.handler.codec.http.HttpDecoderConfig;
import io.netty.handler.codec.http.HttpRequestDecoder;
import io.netty.util.concurrent.DefaultThreadFactory;
import io.netty.util.concurrent.SingleThreadEventExecutor;
import io.netty.util.internal.PlatformDependent;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.context.SmartLifecycle;

import java.lang.management.ManagementFactory;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.concurrent.TimeUnit;

/**
 * Owns the single event-loop thread: it accepts, serves every connection and makes every SQLite call.
 * Start: create the loop, open the database and build the shared request state on it, bind, log one line.
 * Stop: close the listener, let the loop close all connections, commit what is pending, close the database on the
 * loop, terminate.
 */
public final class FeedServer implements SmartLifecycle {

    private static final Logger log = LoggerFactory.getLogger(FeedServer.class);

    static final HttpDecoderConfig DECODER_CONFIG = new HttpDecoderConfig()
            .setMaxInitialLineLength(4096)
            .setMaxHeaderSize(8192)
            .setMaxChunkSize(16384)
            .setHeadersFactory(DefaultHttpHeadersFactory.headersFactory().withValidation(false))
            .setTrailersFactory(DefaultHttpHeadersFactory.trailersFactory().withValidation(false));

    private final FeedProperties properties;
    private final FeedConfig config;
    private final long processStartMillis;

    private volatile boolean running;
    private MultiThreadIoEventLoopGroup group;
    private EventLoop loop;
    private Channel serverChannel;
    private FeedDb db;
    private FeedContext context;

    public FeedServer(FeedProperties properties) {
        this.properties = properties;
        this.config = FeedConfig.fromSystemProperties();
        // JVM start time, not ProcessHandle's: in containers that one is derived from /proc/stat btime, which
        // has whole-second resolution.
        this.processStartMillis = ManagementFactory.getRuntimeMXBean().getStartTime();
    }

    /** The per-connection pipeline. Package-private so tests can build the same one with extra handlers. */
    static void initChild(ChannelPipeline pipeline, FeedContext context) {
        pipeline.addLast(new HttpRequestDecoder(DECODER_CONFIG), new FeedHandler(context));
    }

    /** Creates the shared request state on the loop, after the database is open, and starts the idle sweep. */
    static FeedContext createContext(FeedDb db, EventLoop loop, FeedConfig config, String jwtSecret,
                                     long startMillis) {
        if (!loop.inEventLoop()) {
            throw new IllegalStateException("createContext must run on the event loop");
        }
        FeedContext context = new FeedContext(db, loop, config, jwtSecret.getBytes(StandardCharsets.UTF_8),
                startMillis);
        loop.scheduleAtFixedRate(context.connections::sweep, config.sweepSec(), config.sweepSec(), TimeUnit.SECONDS);
        return context;
    }

    @Override
    public void start() {
        if (!Epoll.isAvailable()) {
            throw new IllegalStateException("epoll transport unavailable", Epoll.unavailabilityCause());
        }

        group = new MultiThreadIoEventLoopGroup(1, new DefaultThreadFactory("feed-loop"), EpollIoHandler.newFactory());
        loop = (EventLoop) group.next();
        try {
            db = new FeedDb(loop);
            context = loop.submit(() -> {
                db.open(Path.of(properties.sqlitePath()));
                return createContext(db, loop, config, properties.jwtSecret(), processStartMillis);
            }).syncUninterruptibly().getNow();
            if (loop instanceof SingleThreadEventExecutor executor) {
                // Shutdown hooks run on the loop after it has closed every registered channel.
                FeedContext c = context;
                executor.addShutdownHook(() -> {
                    try {
                        c.batch.flushNow();
                    } finally {
                        db.close();
                    }
                });
            }

            FeedContext c = context;
            ServerBootstrap bootstrap = new ServerBootstrap()
                    .group(group, group)
                    .channel(EpollServerSocketChannel.class)
                    .option(ChannelOption.SO_BACKLOG, 4096)
                    .option(ChannelOption.SO_REUSEADDR, true)
                    .handler(new AcceptErrorHandler())
                    .childOption(ChannelOption.TCP_NODELAY, true)
                    .childOption(ChannelOption.ALLOCATOR, PooledByteBufAllocator.DEFAULT)
                    .childHandler(new ChannelInitializer<>() {
                        @Override
                        protected void initChannel(Channel ch) {
                            initChild(ch.pipeline(), c);
                        }
                    });
            serverChannel = bootstrap.bind(properties.host(), properties.port()).syncUninterruptibly().channel();
        } catch (Throwable e) {
            // Throwable, not RuntimeException: Netty rethrows bind errors (e.g. EACCES on port 80) as undeclared
            // checked IOExceptions, and a missed one leaves the non-daemon loop thread keeping the JVM alive.
            shutdownGroup();
            throw e;
        }
        running = true;
        String version = loop.submit(db::libVersion).syncUninterruptibly().getNow();
        log.info("listening on {}:{} (epoll, sqlite {}, hasUnsafe={}, startup {} ms; {}; {})",
                properties.host(), properties.port(), version, PlatformDependent.hasUnsafe(),
                System.currentTimeMillis() - processStartMillis, config, context.connections.describeLimit());
    }

    @Override
    public void stop() {
        if (serverChannel != null) {
            serverChannel.close().syncUninterruptibly();
            serverChannel = null;
        }
        shutdownGroup();
        running = false;
    }

    @Override
    public boolean isRunning() {
        return running;
    }

    private void shutdownGroup() {
        if (group == null) {
            return;
        }
        group.shutdownGracefully(0, 5, TimeUnit.SECONDS).syncUninterruptibly();
        group = null;
        if (db != null && db.isOpen()) {
            log.warn("database was not closed on the event loop");
        }
    }
}
