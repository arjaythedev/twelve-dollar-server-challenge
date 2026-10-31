package feed.http;

import io.netty.channel.ChannelConfig;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.util.concurrent.TimeUnit;

/**
 * On the listening channel: accept() errors (EMFILE when out of descriptors) are logged at most once per 10 s and
 * pause accepting for 100 ms instead of reaching the pipeline tail, which would log every one of them.
 */
final class AcceptErrorHandler extends ChannelInboundHandlerAdapter {

    private static final Logger log = LoggerFactory.getLogger(AcceptErrorHandler.class);
    private static final long PAUSE_MILLIS = 100;

    private final RateLimit limit = new RateLimit(10_000);

    @Override
    public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause) {
        if (limit.tryAcquire()) {
            log.warn("accept failed: {} ({} similar suppressed)", cause.toString(), limit.takeSuppressed());
        }
        ChannelConfig config = ctx.channel().config();
        if (config.isAutoRead()) {
            config.setAutoRead(false);
            ctx.channel().eventLoop().schedule(() -> config.setAutoRead(true), PAUSE_MILLIS, TimeUnit.MILLISECONDS);
        }
    }
}
