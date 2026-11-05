package kload

import java.io.IOException
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.StandardSocketOptions
import java.nio.ByteBuffer
import java.nio.channels.SelectionKey
import java.nio.channels.Selector
import java.nio.channels.SocketChannel
import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.util.PriorityQueue
import java.util.SplittableRandom
import java.util.function.Consumer

const val FEED = 0
const val POST = 1
const val LIKE = 2
const val CREATE = 3
val KIND_NAMES = arrayOf("GET /feed", "GET /posts/:id", "POST /posts/:id/like", "POST /posts")

/** Counters owned by one worker thread (other threads only read them, racily, for progress lines). */
class Stats {
    val all = Hist()                  // http_req_duration, whole run (what k6 thresholds use)
    val hold = Hist()                 // hold window only
    val byKind = Array(4) { Hist() }
    val reqs = LongArray(4)
    val fails = LongArray(4)
    var holdReqs = 0L; var holdFails = 0L
    val connect = Hist()              // TCP connect time, successful attempts
    var connAttempts = 0L; var connFails = 0L; var bindFails = 0L
    var timeouts = 0L; var retries = 0L
    var open = 0L                     // currently open sockets
    var bytesIn = 0L; var bytesOut = 0L
    var iterations = 0L; var iterErrors = 0L
    var started = 0L; var finished = 0L
    var maxLoopNs = 0L; var loopStalls = 0L   // generator self-check: event-loop passes that took > 100 ms

    fun merge(o: Stats) {
        maxLoopNs = maxOf(maxLoopNs, o.maxLoopNs); loopStalls += o.loopStalls
        all.merge(o.all); hold.merge(o.hold); connect.merge(o.connect)
        for (k in 0 until 4) { byKind[k].merge(o.byKind[k]); reqs[k] += o.reqs[k]; fails[k] += o.fails[k] }
        holdReqs += o.holdReqs; holdFails += o.holdFails
        connAttempts += o.connAttempts; connFails += o.connFails; bindFails += o.bindFails
        timeouts += o.timeouts; retries += o.retries; open += o.open
        bytesIn += o.bytesIn; bytesOut += o.bytesOut
        iterations += o.iterations; iterErrors += o.iterErrors
        started += o.started; finished += o.finished
    }
}

/** One virtual user: ~100 bytes of state plus its socket. Behaviour mirrors bench/load.js exactly. */
class Vu(val num: Int, val tok: Token, val target: Target, val src: InetAddress?, val srcIdx: Int, val startAt: Long, val stopAt: Long, val hardStop: Long) {
    var ch: SocketChannel? = null
    var key: SelectionKey? = null
    var state = IDLE
    var kind = FEED
    var next = NEXT_FEED
    var wakeAt = 0L
    var postId = 0L
    var iter = 0
    var sendStart = 0L
    var connStart = 0L
    var onConn = 0          // responses received on the current connection
    var retried = false
    var buf: ByteArray? = null
    var len = 0
    var out: ByteBuffer? = null
    var active = false
    var iterPending = false
    var done = false

    companion object {
        const val IDLE = 0; const val CONNECTING = 1; const val AWAITING = 2
        const val NEXT_FEED = 0; const val NEXT_POST = 1; const val NEXT_DECIDE = 2
    }
}

class Worker(private val id: Int, private val cfg: Config, private val t0: Long, val vus: List<Vu>,
             private val portCursors: java.util.concurrent.atomic.AtomicIntegerArray) : Runnable {
    val st = Stats()
    private val sel = Selector.open()
    private val pq = PriorityQueue<Vu>(maxOf(16, vus.size)) { a, b -> a.wakeAt.compareTo(b.wakeAt) }
    private val rnd = SplittableRandom(0x5eed + id.toLong())
    private val rbuf = ByteArray(64 * 1024)
    private val rbb = ByteBuffer.wrap(rbuf)
    private val obuf = ByteBuffer.allocate(16 * 1024)
    private val onKey = Consumer<SelectionKey> { k -> handle(k) }
    private val holdStart = t0 + cfg.rampNs
    private val holdEnd = holdStart + cfg.holdNs
    private val endAt = t0 + cfg.totalNs
    private val iso = DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'").withZone(ZoneOffset.UTC)

    // Results of the last parse().
    private var pStatus = 0
    private var pClose = false
    private var pBodyOff = 0
    private var pBodyLen = 0
    private var pChunked = false

    companion object {
        private val GET_ = "GET ".toByteArray()
        private val POST_ = "POST ".toByteArray()
        private val FEED_PATH = "/feed".toByteArray()
        private val POSTS_PATH = "/posts".toByteArray()
        private val SLASH = "/".toByteArray()
        private val LIKE_PATH = "/like".toByteArray()
        private val HTTP11 = " HTTP/1.1\r\n".toByteArray()
        private val CL0 = "Content-Length: 0\r\n\r\n".toByteArray()
        private val CL = "Content-Length: ".toByteArray()
        private val CRLF2 = "\r\n\r\n".toByteArray()
        private val CRLF = "\r\n".toByteArray()
        private val ID_MARK = "{\"id\":".toByteArray()
    }

    override fun run() {
        for (v in vus) {
            // k6: `if (__ITER === 0) sleep(between(0, 5))` once the VU has been started by the ramp.
            v.wakeAt = v.startAt + (rnd.nextDouble() * 5e9).toLong()
            v.next = Vu.NEXT_FEED
            pq.add(v)
        }
        var nextSweep = System.nanoTime() + 1_000_000_000L
        var loopStart = System.nanoTime()
        while (true) {
            var now = System.nanoTime()
            if (now >= endAt) break
            while (true) {
                val v = pq.peek() ?: break
                if (v.wakeAt > now) break
                pq.poll()
                wake(v, now)
                now = System.nanoTime()
            }
            if (now >= nextSweep) { sweep(now); nextSweep = now + 1_000_000_000L }
            val nextT = minOf(pq.peek()?.wakeAt ?: Long.MAX_VALUE, nextSweep, endAt)
            // Round the wait UP: think times are seconds long, so firing up to 1 ms late is harmless, while
            // rounding down would busy-spin selectNow() whenever the next timer is under 1 ms away.
            val waitNs = nextT - now
            val busy = now - loopStart   // time spent working since the last select returned
            if (busy > st.maxLoopNs) st.maxLoopNs = busy
            if (busy > 100_000_000L) st.loopStalls++
            if (waitNs <= 0) sel.selectNow(onKey) else sel.select(onKey, minOf((waitNs + 999_999L) / 1_000_000L, 1000L))
            loopStart = System.nanoTime()
        }
        for (v in vus) closeConn(v)
        sel.close()
    }

    // ------------------------------------------------------------------ VU script (bench/load.js)

    private fun wake(v: Vu, now: Long) {
        if (!v.active) { v.active = true; st.started++ }
        if (now >= v.hardStop) return finish(v)                         // interrupted: k6 doesn't count it
        if (v.iterPending) { v.iterPending = false; st.iterations++ }   // k6 counts an iteration once its last sleep ends
        when (v.next) {
            Vu.NEXT_FEED -> if (now >= v.stopAt) finish(v) else request(v, FEED)
            Vu.NEXT_POST -> request(v, POST)
            Vu.NEXT_DECIDE -> if (rnd.nextDouble() < cfg.likeProb) request(v, LIKE) else afterLike(v)
        }
    }

    /** Called once per request, after metrics were recorded. ids: post ids found in a feed body. */
    private fun continueAfter(v: Vu, status: Int, ids: LongArray?, nIds: Int) {
        when (v.kind) {
            FEED -> when {
                status != 200 -> { endIter(v); sleep(v, 3.0, 7.0, Vu.NEXT_FEED) }   // check failed: sleep, return
                nIds == 0 -> { st.iterErrors++; endIter(v); sleep(v, 0.0, 0.0, Vu.NEXT_FEED) } // feed.json() threw
                else -> { v.postId = ids!![rnd.nextInt(nIds)]; sleep(v, 3.0, 7.0, Vu.NEXT_POST) }
            }
            POST -> sleep(v, 3.0, 8.0, Vu.NEXT_DECIDE)
            LIKE -> afterLike(v)
            CREATE -> { endIter(v); sleep(v, 5.0, 15.0, Vu.NEXT_FEED) }
        }
    }

    private fun afterLike(v: Vu) {
        if (rnd.nextDouble() < cfg.postProb) request(v, CREATE)
        else { endIter(v); sleep(v, 5.0, 15.0, Vu.NEXT_FEED) }
    }

    private fun endIter(v: Vu) { v.iter++; v.iterPending = true }

    private fun sleep(v: Vu, lo: Double, hi: Double, next: Int) {
        v.next = next
        v.wakeAt = System.nanoTime() + ((lo + rnd.nextDouble() * (hi - lo)) * 1e9).toLong()
        pq.add(v)
    }

    private fun finish(v: Vu) {
        if (v.done) return
        v.done = true; st.finished++
        closeConn(v)
    }

    // ------------------------------------------------------------------ HTTP client

    private fun request(v: Vu, kind: Int) {
        v.kind = kind
        v.sendStart = 0
        v.retried = false
        if (v.ch == null) connect(v) else send(v)
    }

    private fun connect(v: Vu) {
        st.connAttempts++
        v.connStart = System.nanoTime()
        val ch: SocketChannel
        try {
            ch = SocketChannel.open()
            v.ch = ch; st.open++
            ch.configureBlocking(false)
            ch.setOption(StandardSocketOptions.TCP_NODELAY, true)
        } catch (e: IOException) {
            st.connFails++; closeConn(v); return fail(v)
        }
        try {
            if (v.src != null) bindPort(ch, v)
        } catch (e: IOException) {
            st.connFails++; st.bindFails++; closeConn(v); return fail(v)   // no free local port on this source IP
        }
        try {
            v.onConn = 0
            if (ch.connect(v.target.addr)) connected(v)
            else { v.key = ch.register(sel, SelectionKey.OP_CONNECT, v); v.state = Vu.CONNECTING }
        } catch (e: IOException) {
            st.connFails++; closeConn(v); fail(v)
        }
    }

    /**
     * Binds to an explicit source port. bind(ip, 0) makes the kernel scan for a free port, and that scan gets
     * slower as the IP fills up (at ~24k sockets per IP it stalled every event loop for tens of seconds). An
     * explicit port is one hash lookup. All threads share one cursor per source IP, which walks 1024..65535 and
     * skips taken ports (EADDRINUSE). A per-thread port slice broke when the VU->IP and VU->thread mappings
     * lined up (4 IPs, 4 threads: each thread had one IP and only 1/4 of its ports).
     */
    private fun bindPort(ch: SocketChannel, v: Vu) {
        val i = v.srcIdx
        var tries = 0
        while (true) {
            val p = 1024 + ((portCursors.getAndIncrement(i) and 0x7fffffff) % 64512)
            try {
                ch.bind(InetSocketAddress(v.src, p))
                return
            } catch (e: java.net.BindException) {
                if (++tries >= 64512) throw e   // this source IP is out of ports
            }
        }
    }

    private fun connected(v: Vu) {
        st.connect.add((System.nanoTime() - v.connStart) / 1000)
        val ch = v.ch!!
        val k = v.key
        if (k == null) v.key = ch.register(sel, SelectionKey.OP_READ, v) else k.interestOps(SelectionKey.OP_READ)
        send(v)
    }

    private fun send(v: Vu) {
        val b = obuf
        b.clear()
        val t = v.target
        when (v.kind) {
            FEED -> { b.put(GET_); b.put(t.prefix); b.put(FEED_PATH); b.put(HTTP11); b.put(t.hostLine); b.put(CRLF) }
            POST -> { b.put(GET_); b.put(t.prefix); b.put(POSTS_PATH); b.put(SLASH); putNum(b, v.postId); b.put(HTTP11); b.put(t.hostLine); b.put(CRLF) }
            LIKE -> {
                b.put(POST_); b.put(t.prefix); b.put(POSTS_PATH); b.put(SLASH); putNum(b, v.postId); b.put(LIKE_PATH); b.put(HTTP11)
                b.put(t.hostLine); b.put(v.tok.authLine); b.put(CL0)
            }
            CREATE -> {
                // JSON.stringify({ body: `${user.username} says hi at ${new Date().toISOString()} (VU ${__VU}, iter ${__ITER})` })
                val body = "{\"body\":\"${v.tok.username} says hi at ${iso.format(Instant.now())} (VU ${v.num}, iter ${v.iter})\"}".toByteArray()
                b.put(POST_); b.put(t.prefix); b.put(POSTS_PATH); b.put(HTTP11); b.put(t.hostLine); b.put(v.tok.authLine)
                b.put(CL); putNum(b, body.size.toLong()); b.put(CRLF2); b.put(body)
            }
        }
        b.flip()
        st.bytesOut += b.remaining()
        v.state = Vu.AWAITING
        v.sendStart = System.nanoTime()
        try {
            v.ch!!.write(b)
        } catch (e: IOException) {
            return netError(v)
        }
        if (b.hasRemaining()) {
            val o = ByteBuffer.allocate(b.remaining()); o.put(b); o.flip()
            v.out = o
            v.key!!.interestOps(SelectionKey.OP_READ or SelectionKey.OP_WRITE)
        }
    }

    private fun putNum(b: ByteBuffer, n: Long) {
        if (n >= 10) putNum(b, n / 10)
        b.put(('0'.code + (n % 10).toInt()).toByte())
    }

    private fun handle(k: SelectionKey) {
        val v = k.attachment() as Vu
        if (!k.isValid) return
        val ops = k.readyOps()
        if (ops and SelectionKey.OP_CONNECT != 0) {
            try {
                if (v.ch!!.finishConnect()) connected(v)
            } catch (e: IOException) {
                st.connFails++; closeConn(v); fail(v)
            }
            return
        }
        if (ops and SelectionKey.OP_WRITE != 0) {
            val o = v.out
            if (o != null) {
                try { v.ch!!.write(o) } catch (e: IOException) { return netError(v) }
                if (!o.hasRemaining()) { v.out = null; k.interestOps(SelectionKey.OP_READ) }
            } else k.interestOps(SelectionKey.OP_READ)
        }
        if (ops and SelectionKey.OP_READ != 0 && v.ch != null) readable(v)
    }

    private fun readable(v: Vu) {
        rbb.clear()
        val n = try { v.ch!!.read(rbb) } catch (e: IOException) { -1 }
        if (n > 0) st.bytesIn += n
        if (v.state != Vu.AWAITING) {             // idle keep-alive socket: server closed it, or stray bytes
            if (n < 0) closeConn(v)
            return
        }
        val eof = n < 0
        val arr: ByteArray
        val len: Int
        if (v.buf == null) { arr = rbuf; len = maxOf(n, 0) }
        else {
            if (n > 0) append(v, rbuf, n)
            arr = v.buf!!; len = v.len
        }
        if (len == 0) { if (eof) netError(v); return }
        val total = parse(arr, len, eof)
        if (total == 0) {
            if (eof) { closeConn(v); return fail(v) }   // partial response, then the server hung up
            if (arr === rbuf) { v.buf = null; v.len = 0; append(v, rbuf, len) }
            return
        }
        if (total < 0) { closeConn(v); return fail(v) }

        val status = pStatus
        var ids: LongArray? = null
        var nIds = 0
        if (v.kind == FEED && status == 200) {
            val body: ByteArray; val off: Int; val blen: Int
            if (pChunked) { body = dechunk(arr, pBodyOff, total); off = 0; blen = body.size } else { body = arr; off = pBodyOff; blen = pBodyLen }
            ids = LongArray(32)
            var i = off
            val end = off + blen
            while (i + ID_MARK.size < end && nIds < ids.size) {
                if (match(body, i, ID_MARK)) {
                    var j = i + ID_MARK.size; var id = 0L
                    while (j < end && body[j] >= '0'.code.toByte() && body[j] <= '9'.code.toByte()) { id = id * 10 + (body[j] - '0'.code.toByte()); j++ }
                    if (j > i + ID_MARK.size) ids[nIds++] = id
                    i = j
                } else i++
            }
        }
        val closeAfter = pClose || eof
        v.buf = null; v.len = 0
        v.onConn++
        v.state = Vu.IDLE
        record(v, status)
        if (closeAfter) closeConn(v)
        continueAfter(v, status, ids, nIds)
    }

    /** Connection broke while a request was in flight. Like Go's transport (k6), a request that failed on a
     *  reused keep-alive connection before any response byte arrived is retried once on a fresh connection. */
    private fun netError(v: Vu) {
        val reused = v.onConn > 0
        closeConn(v)
        if (reused && !v.retried) {
            v.retried = true; st.retries++
            connect(v)
            return
        }
        fail(v)
    }

    private fun fail(v: Vu) {
        v.state = Vu.IDLE
        v.buf = null; v.len = 0
        record(v, 0)
        continueAfter(v, 0, null, 0)
    }

    private fun record(v: Vu, status: Int) {
        val now = System.nanoTime()
        val us = if (v.sendStart > 0) (now - v.sendStart) / 1000 else 0
        val failed = status < 200 || status >= 400
        val k = v.kind
        st.all.add(us); st.byKind[k].add(us)
        st.reqs[k]++
        if (failed) st.fails[k]++
        if (now in holdStart until holdEnd) { st.hold.add(us); st.holdReqs++; if (failed) st.holdFails++ }
    }

    private fun closeConn(v: Vu) {
        val ch = v.ch ?: return
        v.key?.cancel()
        try { ch.close() } catch (_: IOException) {}
        v.ch = null; v.key = null; v.out = null
        st.open--
    }

    /** Request timeouts (k6 default 60 s). */
    private fun sweep(now: Long) {
        for (v in vus) {
            val t = when (v.state) {
                Vu.AWAITING -> v.sendStart
                Vu.CONNECTING -> v.connStart
                else -> continue
            }
            if (now - t > cfg.timeoutNs) {
                st.timeouts++
                v.retried = true // no retry after a timeout
                closeConn(v); fail(v)
            }
        }
    }

    private fun append(v: Vu, src: ByteArray, n: Int) {
        var b = v.buf
        if (b == null) b = ByteArray(maxOf(8192, n))
        if (v.len + n > b.size) b = b.copyOf(maxOf(b.size * 2, v.len + n))
        System.arraycopy(src, 0, b, v.len, n)
        v.buf = b; v.len += n
    }

    // ------------------------------------------------------------------ HTTP/1.1 response parsing

    /** Returns total response length, 0 if incomplete, -1 if malformed. Sets pStatus, pClose, pBodyOff, pBodyLen, pChunked. */
    private fun parse(b: ByteArray, len: Int, eof: Boolean): Int {
        var he = -1
        var i = 0
        while (i + 3 < len) {
            if (b[i] == '\r'.code.toByte() && b[i + 1] == '\n'.code.toByte() && b[i + 2] == '\r'.code.toByte() && b[i + 3] == '\n'.code.toByte()) { he = i + 4; break }
            i++
        }
        if (he < 0) return if (eof || len > 64 * 1024) -1 else 0
        if (len < 12 || b[0] != 'H'.code.toByte()) return -1
        pStatus = (b[9] - 48) * 100 + (b[10] - 48) * 10 + (b[11] - 48)
        val http10 = b[7] == '0'.code.toByte()
        pClose = http10
        pChunked = false
        var cl = -1L
        var p = 0
        while (p < he && b[p] != '\n'.code.toByte()) p++
        p++
        while (p < he - 2) {
            var le = p
            while (b[le] != '\r'.code.toByte()) le++
            var colon = p
            while (colon < le && b[colon] != ':'.code.toByte()) colon++
            var vs = colon + 1
            while (vs < le && b[vs] == ' '.code.toByte()) vs++
            when (colon - p) {
                14 -> if (ieq(b, p, "content-length")) { cl = 0; for (k in vs until le) { val d = b[k] - 48; if (d in 0..9) cl = cl * 10 + d } }
                17 -> if (ieq(b, p, "transfer-encoding") && icontains(b, vs, le, "chunked")) pChunked = true
                10 -> if (ieq(b, p, "connection")) {
                    if (icontains(b, vs, le, "close")) pClose = true else if (icontains(b, vs, le, "keep-alive")) pClose = false
                }
            }
            p = le + 2
        }
        pBodyOff = he
        if (pStatus in 100..199 || pStatus == 204 || pStatus == 304) { pBodyLen = 0; return he }
        if (pChunked) {
            var pos = he
            while (true) {
                var le = pos
                while (le + 1 < len && !(b[le] == '\r'.code.toByte() && b[le + 1] == '\n'.code.toByte())) le++
                if (le + 1 >= len) return if (eof) -1 else 0
                var size = 0L
                for (k in pos until le) {
                    val c = b[k].toInt().toChar()
                    val d = Character.digit(c, 16)
                    if (d < 0) break
                    size = size * 16 + d
                }
                if (size == 0L) {
                    // trailers end with an empty line
                    var q = le + 2
                    while (true) {
                        if (q + 1 >= len) return if (eof) -1 else 0
                        if (b[q] == '\r'.code.toByte() && b[q + 1] == '\n'.code.toByte()) return q + 2
                        while (q + 1 < len && !(b[q] == '\r'.code.toByte() && b[q + 1] == '\n'.code.toByte())) q++
                        q += 2
                    }
                }
                pos = le + 2 + size.toInt() + 2
                if (pos > len) return if (eof) -1 else 0
            }
        }
        if (cl >= 0) {
            pBodyLen = cl.toInt()
            val total = he + pBodyLen
            return if (len >= total) total else if (eof) -1 else 0
        }
        // No length: body runs until the server closes the connection.
        pClose = true
        pBodyLen = len - he
        return if (eof) len else 0
    }

    private fun dechunk(b: ByteArray, from: Int, end: Int): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        var pos = from
        while (pos < end) {
            var le = pos
            while (le + 1 < end && !(b[le] == '\r'.code.toByte() && b[le + 1] == '\n'.code.toByte())) le++
            var size = 0
            for (k in pos until le) { val d = Character.digit(b[k].toInt().toChar(), 16); if (d < 0) break; size = size * 16 + d }
            if (size == 0) break
            out.write(b, le + 2, size)
            pos = le + 2 + size + 2
        }
        return out.toByteArray()
    }

    private fun match(b: ByteArray, at: Int, lit: ByteArray): Boolean {
        for (k in lit.indices) if (b[at + k] != lit[k]) return false
        return true
    }

    private fun ieq(b: ByteArray, s: Int, lit: String): Boolean {
        for (k in lit.indices) if ((b[s + k].toInt() or 0x20) != lit[k].code) return false
        return true
    }

    private fun icontains(b: ByteArray, s: Int, e: Int, lit: String): Boolean {
        var i = s
        while (i + lit.length <= e) { if (ieq(b, i, lit)) return true; i++ }
        return false
    }
}
