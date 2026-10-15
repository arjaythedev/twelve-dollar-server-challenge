package borg.trikeshed.feed

/** A connected socket of the platform: a java.nio SocketChannel on the JVM, a descriptor on native targets. */
expect class Handle

/**
 * The platform's readiness poller around one listening socket: java.nio's Selector on the JVM, epoll/kqueue over
 * descriptors on native targets. [select] calls back into [Xio] directly for each ready handle.
 */
expect class Poller(host: String, port: Int) {
    /**
     * Blocks until handles are ready (at most [timeoutMs] when it is > 0), then calls [Xio.onAccept] for the listener
     * and [Xio.ready] per connection.
     */
    fun select(xio: Xio, timeoutMs: Long)

    /** The next pending connection, or null when none is waiting. */
    fun accept(): Handle?

    /** Starts read-readiness reporting for [v]; closes it when the platform refuses. */
    fun register(v: Visitor)

    /** Adds or removes write-readiness reporting for [v]. */
    fun wantWrite(v: Visitor, on: Boolean)

    /** Reads at most [len] bytes into [dst] at [off]: the count, 0 when none are ready, -1 at end of stream or error. */
    fun read(h: Handle, dst: ByteArray, off: Int, len: Int): Int

    /** Writes at most [len] bytes from [src] at [off]: the count (0 when the socket buffer is full), -1 on error. */
    fun write(h: Handle, src: ByteArray, off: Int, len: Int): Int

    fun close(v: Visitor)
}

/** Seconds since the epoch. */
expect fun epochSeconds(): Long

/** A monotonic clock in nanoseconds. */
expect fun monoNanos(): Long

/**
 * How long the write transaction stays open for more writes before its COMMIT. At 64,000 users on a 1-CPU container
 * (2 min holds) server CPU per request was 67.1-69.6 us with one COMMIT per selector pass, 59.7-62.1 us with a 32 ms
 * window, 56.2-59.0 us with 64 ms and 56.3-56.4 us with 128 ms (README, Group commit).
 */
const val COMMIT_WINDOW_NANOS = 64_000_000L

/**
 * RelaxFactory's AsioVisitor (one.xio) as one final class: one per connection. RelaxFactory swaps visitor subclasses
 * per connection state; here the state is fields: [cursor] holds request bytes across reads
 * (RequestQueueVisitor.cursor), [payload] holds response bytes across writes (RequestQueueVisitor.payload).
 */
class Visitor(val handle: Handle) {
    var cursor: ByteArray? = null
    var cursorSize = 0
    var payload: ByteArray? = null
    var payloadFrom = 0
    /** Close after the last response is written (Connection: close, HTTP/1.0). */
    var closing = false
    var open = true
    /** Responses are waiting for the open write transaction's COMMIT. */
    var deferred = false
}

/**
 * RelaxFactory's one.xio.HttpMethod.init/innerloop, monomorphized, over the platform [Poller]. One thread owns the
 * SQLite connection; nothing registers from other threads, so there is no enqueue queue. Writes join one open
 * transaction until it is [commitWindowNanos] old; every response produced while it is open waits for its COMMIT
 * (group commit), and select waits at most until the window closes.
 */
class Xio(val poller: Poller, val feed: Feed, val store: FeedStore, val commitWindowNanos: Long = COMMIT_WINDOW_NANOS) {
    val rx = ByteArray(1 shl 16)
    val request = Request()
    val send = Out()
    val deferred = ArrayList<Visitor>()
    /** Whether the end of a pass has seen the open write transaction, and when ([monoNanos]). */
    var windowOpen = false
    var openedAt = 0L

    fun init() {
        while (true) {
            val timeoutMs = if (windowOpen) maxOf(1L, (openedAt + commitWindowNanos - monoNanos() + 999_999L) / 1_000_000L) else 0L
            poller.select(this, timeoutMs)
            if (!store.pending) continue
            val now = monoNanos()
            if (!windowOpen) { windowOpen = true; openedAt = now }
            if (now - openedAt >= commitWindowNanos) { windowOpen = false; settle() }
        }
    }

    fun onAccept() {
        while (true) poller.register(Visitor(poller.accept() ?: return))
    }

    fun ready(v: Visitor, writable: Boolean, readable: Boolean) {
        try {
            if (writable) onWrite(v)
            if (readable && v.open) onRead(v)
        } catch (_: Throwable) {
            close(v)
        }
    }

    fun close(v: Visitor) {
        if (!v.open) return
        v.open = false
        poller.close(v)
    }

    fun onRead(v: Visitor) {
        val n = poller.read(v.handle, rx, 0, rx.size)
        if (n < 0) { close(v); return }
        if (n == 0) return
        val held = v.cursor
        val src: ByteArray
        val to: Int
        if (held == null) {
            src = rx
            to = n
        } else {
            val grown = if (v.cursorSize + n > held.size) held.copyOf(maxOf(held.size * 2, v.cursorSize + n)) else held
            rx.copyInto(grown, v.cursorSize, 0, n)
            src = grown
            to = v.cursorSize + n
        }
        send.size = 0
        var from = 0
        while (from < to && !v.closing) {
            val end = request.frame(src, from, to)
            if (end == 0) break
            if (end < 0) { close(v); return }
            feed.respond(src, request, send)
            if (request.close) v.closing = true
            from = end
        }
        if (from < to && !v.closing) {
            v.cursor = if (src === rx) src.copyOfRange(from, to) else src.copyInto(src, 0, from, to)
            v.cursorSize = to - from
        } else {
            v.cursor = null
            v.cursorSize = 0
        }
        if (send.size == 0) return
        if (store.pending) {
            hold(v, send.bytes, 0, send.size)
            if (!v.deferred) { v.deferred = true; deferred.add(v) }
        } else if (v.payload != null) {
            hold(v, send.bytes, 0, send.size) // behind bytes still waiting for write readiness
        } else {
            transmit(v, send.bytes, 0, send.size)
        }
    }

    fun hold(v: Visitor, bytes: ByteArray, from: Int, to: Int) {
        val prior = v.payload
        v.payload = if (prior == null) bytes.copyOfRange(from, to) else prior.copyOfRange(v.payloadFrom, prior.size) + bytes.copyOfRange(from, to)
        v.payloadFrom = 0
    }

    /** Commit the open write transaction, then release every response that waited on it. */
    fun settle() {
        val committed = try { store.commit(); true } catch (_: Throwable) { false }
        for (i in deferred.indices) {
            val v = deferred[i]
            v.deferred = false
            if (!v.open) continue
            if (!committed) { close(v); continue }
            val p = v.payload ?: continue
            v.payload = null
            transmit(v, p, v.payloadFrom, p.size)
        }
        deferred.clear()
    }

    fun transmit(v: Visitor, bytes: ByteArray, from: Int, to: Int) {
        var at = from
        while (at < to) {
            val w = poller.write(v.handle, bytes, at, to - at)
            if (w < 0) { close(v); return }
            if (w == 0) break
            at += w
        }
        if (at < to) {
            hold(v, bytes, at, to)
            poller.wantWrite(v, true)
        } else if (v.closing) close(v)
    }

    fun onWrite(v: Visitor) {
        if (v.deferred) return
        poller.wantWrite(v, false)
        val p = v.payload ?: return
        v.payload = null
        transmit(v, p, v.payloadFrom, p.size)
    }
}
