package feed.http;

import feed.auth.JwtVerifier;
import feed.db.DbException;
import feed.db.FeedDb;
import feed.json.BodyParser;
import io.netty.channel.EventLoop;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.util.HashSet;
import java.util.Set;

/**
 * Everything the per-connection handlers share. One instance per event loop, created and used only on it; the
 * JWT verifier, body parser, sink and scratch array are reused by every request, which is safe because a request
 * is handled to completion before the next one starts.
 */
final class FeedContext {

    private static final Logger log = LoggerFactory.getLogger(FeedContext.class);

    final FeedDb db;
    final Batch batch;
    final Connections connections;
    final JwtVerifier jwt;
    final BodyParser parser = new BodyParser();
    final ResponseSink sink = new ResponseSink();
    /** Copy of a request body that arrived in a direct buffer, for the byte[] parser. */
    final byte[] bodyScratch = new byte[FeedHandler.MAX_BODY];
    final long startMillis;

    private final RateLimit dbErrorLimit = new RateLimit(10_000);
    private final Set<Integer> seenErrorCodes = new HashSet<>();

    FeedContext(FeedDb db, EventLoop loop, FeedConfig config, byte[] jwtSecret, long startMillis) {
        this.db = db;
        this.batch = new Batch(db, loop, config);
        this.connections = new Connections(config, batch);
        this.jwt = new JwtVerifier(jwtSecret);
        this.startMillis = startMillis;
    }

    /** Logs a failed request statement: the first time per extended code, then at most once per 10 s. */
    void dbError(String operation, RuntimeException e) {
        int code = e instanceof DbException d ? d.extendedCode() : -1;
        boolean first = seenErrorCodes.size() < 64 && seenErrorCodes.add(code);
        if (first || dbErrorLimit.tryAcquire()) {
            log.warn("{} failed: {} ({} similar suppressed)", operation, e.toString(), dbErrorLimit.takeSuppressed());
        }
        batch.onDbError();
    }
}
