package nio

import java.io.IOException
import java.net.InetSocketAddress
import java.net.StandardSocketOptions
import java.nio.ByteBuffer
import java.nio.channels.SelectionKey
import java.nio.channels.Selector
import java.nio.channels.ServerSocketChannel
import java.nio.channels.SocketChannel
import java.util.function.Consumer

class Conn(val ch: SocketChannel) {
    lateinit var key: SelectionKey
    var pending: ByteArray? = null   // unparsed input (partial or pipelined requests)
    var pendingLen = 0
    var out: ByteBuffer? = null      // output the socket didn't take yet (read mode)
    var busy = false                 // has a write waiting for the group commit
    var closeAfter = false
    var closed = false
}

class WriteOp(
    val conn: Conn, val isLike: Boolean, val userId: Long, val postId: Long,
    val body: String?, val author: String?, val close: Boolean,
)

/**
 * Single-threaded HTTP/1.1 server: one epoll selector, one SQLite connection, no framework.
 * Reads are answered inline. Writes are queued and committed together once per loop
 * iteration (group commit); their responses go out only after COMMIT returns.
 */
class Server(private val host: String, private val port: Int, private val db: Db, private val jwt: Jwt) {
    private val sel = Selector.open()
    private lateinit var ss: ServerSocketChannel
    private lateinit var acceptKey: SelectionKey
    private var acceptPaused = false
    private val rbuf = ByteArray(64 * 1024)
    private val rbb = ByteBuffer.wrap(rbuf)
    private val obuf = ByteBuffer.allocateDirect(1 shl 20)
    private var writes = ArrayList<WriteOp>(1024)
    private var spare = ArrayList<WriteOp>(1024)
    private val started = System.nanoTime()
    private val onKey = Consumer<SelectionKey> { k -> handleKey(k) }
    private val num = ByteArray(24)
    private var lockBusy = false
    private var busySince = 0L
    private var firstWriteNs = 0L   // when the oldest queued write arrived
    private var keysThisPass = 0

    companion object {
        private const val GET = 1
        private const val POST = 2
        private const val MAX_REQ = 60 * 1024

        // Loop pacing (LOCAL A/B, 2 rounds at 128k users: 47.2 -> 41.6 us/req; 192k: 42.8 -> 40.7 us/req, p99 57 -> 32 ms).
        // Group commit waits until the oldest queued write is WRITE_WINDOW_NS old or BATCH_MAX writes are queued,
        // so one COMMIT (and one set of WAL frames) serves more writes. After a light pass (< LIGHT_PASS ready keys,
        // nothing queued) the loop parks PACE_NS so the next select gathers more sockets per syscall; under load
        // passes are heavy and it never parks.
        private const val WRITE_WINDOW_NS = 2_000_000L
        private const val BATCH_MAX = 64
        private const val PACE_NS = 1_000_000L
        private const val LIGHT_PASS = 32

        private fun a(s: String) = s.toByteArray(Charsets.UTF_8)
        private fun head(code: String) = a("HTTP/1.1 $code\r\nContent-Type: application/json\r\nContent-Length: ")
        private val H200 = head("200 OK")
        private val H201 = head("201 Created")
        private val H400 = head("400 Bad Request")
        private val H401 = head("401 Unauthorized")
        private val H404 = head("404 Not Found")
        private val H500 = head("500 Internal Server Error")
        private val H503 = head("503 Service Unavailable")
        private val CRLF2 = a("\r\n\r\n")
        private val CLOSE = a("\r\nConnection: close\r\n\r\n")

        private val E_NOT_FOUND = a("{\"error\":\"not found\"}")
        private val E_BAD_ID = a("{\"error\":\"invalid post id\"}")
        private val E_NO_POST = a("{\"error\":\"post not found\"}")
        private val E_MISSING = a("{\"error\":\"missing bearer token\"}")
        private val E_INVALID = a("{\"error\":\"invalid or expired token\"}")
        private val E_PAYLOAD = a("{\"error\":\"invalid token payload\"}")
        private val E_JSON = a("{\"error\":\"malformed JSON body\"}")
        private val E_BODY = a("{\"error\":\"body is required\"}")
        private val E_LONG = a("{\"error\":\"body must be at most 500 characters\"}")
        private val E_500 = a("{\"error\":\"internal server error\"}")

        private val P_HEALTH = a("/health")
        private val P_FEED = a("/feed")
        private val P_POSTS = a("/posts")
        private val P_LIKE = a("/like")
        private val LIKED_NEW = a("{\"liked\":true,\"already_liked\":false,\"post_id\":")
        private val LIKED_OLD = a("{\"liked\":true,\"already_liked\":true,\"post_id\":")
    }

    fun run() {
        ss = ServerSocketChannel.open()
        ss.setOption(StandardSocketOptions.SO_REUSEADDR, true)
        // Several worker processes bind the same port; the kernel spreads connections across them.
        // Each process has its own RLIMIT_NOFILE, so N workers can hold N x 65,535 connections.
        ss.setOption(StandardSocketOptions.SO_REUSEPORT, true)
        ss.bind(InetSocketAddress(host, port), 65535)
        ss.configureBlocking(false)
        acceptKey = ss.register(sel, SelectionKey.OP_ACCEPT)
        println("worker ${System.getenv("WORKER_ID") ?: "0"} listening on $host:$port")
        while (true) {
            keysThisPass = 0
            when {
                writes.isEmpty() -> sel.select(onKey)
                lockBusy -> sel.select(onKey, 1) // another worker holds the write lock: keep serving, retry in ~1 ms
                else -> {
                    val left = WRITE_WINDOW_NS - (System.nanoTime() - firstWriteNs)
                    if (left <= 0 || writes.size >= BATCH_MAX) sel.selectNow(onKey)
                    else sel.select(onKey, (left + 999_999) / 1_000_000)   // wait for the window (or new input)
                }
            }
            if (writes.isNotEmpty() && (lockBusy || writes.size >= BATCH_MAX || System.nanoTime() - firstWriteNs >= WRITE_WINDOW_NS)) runBatch()
            if (keysThisPass < LIGHT_PASS && writes.isEmpty()) java.util.concurrent.locks.LockSupport.parkNanos(PACE_NS)
        }
    }

    private fun handleKey(k: SelectionKey) {
        keysThisPass++
        if (!k.isValid) return
        val ops = k.readyOps()
        if (ops and SelectionKey.OP_ACCEPT != 0) { accept(); return }
        val c = k.attachment() as Conn
        try {
            if (ops and SelectionKey.OP_WRITE != 0) onWritable(c)
            if (!c.closed && ops and SelectionKey.OP_READ != 0) onReadable(c)
        } catch (e: Exception) {
            close(c)
        }
    }

    private fun accept() {
        while (true) {
            val ch = try { ss.accept() } catch (e: IOException) {
                // Out of file descriptors (EMFILE): the listen socket stays readable, so stop polling it
                // until a connection closes instead of spinning on it.
                if (!acceptPaused) System.err.println("accept paused: $e")
                acceptPaused = true
                acceptKey.interestOps(0)
                return
            } ?: return
            try {
                ch.configureBlocking(false)
                ch.setOption(StandardSocketOptions.TCP_NODELAY, true)
                val c = Conn(ch)
                c.key = ch.register(sel, SelectionKey.OP_READ, c)
            } catch (e: IOException) {
                try { ch.close() } catch (_: IOException) {}
            }
        }
    }

    private fun close(c: Conn) {
        if (c.closed) return
        c.closed = true
        c.key.cancel()
        try { c.ch.close() } catch (_: IOException) {}
        c.pending = null; c.out = null
        if (acceptPaused) { acceptPaused = false; acceptKey.interestOps(SelectionKey.OP_ACCEPT) }
    }

    // ---------------------------------------------------------------- input

    private fun onReadable(c: Conn) {
        var len = takePending(c)
        rbb.limit(rbuf.size).position(len)
        val n = try { c.ch.read(rbb) } catch (e: IOException) { -1 }
        if (n < 0) { close(c); return }
        len += n
        process(c, len)
    }

    /** Moves c's stashed input to the front of rbuf; returns its length. */
    private fun takePending(c: Conn): Int {
        val p = c.pending ?: return 0
        System.arraycopy(p, 0, rbuf, 0, c.pendingLen)
        c.pending = null
        return c.pendingLen
    }

    /** Handles every complete request in rbuf[0, len), flushes, and stashes the remainder. */
    private fun process(c: Conn, len: Int) {
        var pos = 0
        while (pos < len && !c.busy && !c.closeAfter) {
            val used = request(c, pos, len)
            if (used < 0) { flush(c); close(c); return }
            if (used == 0) break
            pos += used
        }
        flush(c)
        if (c.closed) return
        if (pos < len) {
            if (len - pos >= MAX_REQ) { close(c); return }
            val rest = len - pos
            val p = c.pending.let { if (it != null && it.size >= rest) it else ByteArray(maxOf(rest, 1024)) }
            System.arraycopy(rbuf, pos, p, 0, rest)
            c.pending = p; c.pendingLen = rest
        }
    }

    /** Parses one request at rbuf[s, e). Returns bytes consumed, 0 if incomplete, -1 to drop the connection. */
    private fun request(c: Conn, s: Int, e: Int): Int {
        val b = rbuf
        // One pass over the head: the request line, then header lines up to the empty line. Only lines whose name
        // starts with c, a or t (Content-Length, Connection, Authorization, Transfer-Encoding; any case, any order)
        // are looked at; every other line costs only the scan for its '\n'. Every client is parsed the same way.
        var rl = s
        while (rl < e && b[rl] != '\n'.code.toByte()) rl++
        if (rl >= e) return if (e - s > 16 * 1024) -1 else 0

        // Request line.
        var method = 0
        var p: Int
        if (e - s > 4 && b[s] == 'G'.code.toByte() && b[s + 1] == 'E'.code.toByte() && b[s + 2] == 'T'.code.toByte() && b[s + 3] == ' '.code.toByte()) {
            method = GET; p = s + 4
        } else if (e - s > 5 && b[s] == 'P'.code.toByte() && b[s + 1] == 'O'.code.toByte() && b[s + 2] == 'S'.code.toByte() && b[s + 3] == 'T'.code.toByte() && b[s + 4] == ' '.code.toByte()) {
            method = POST; p = s + 5
        } else {
            p = s
            while (p < rl && b[p] != ' '.code.toByte()) p++
            p++
        }
        val ts = p
        while (p < rl && b[p] != ' '.code.toByte() && b[p] != '\r'.code.toByte()) p++
        val te = p
        var pathEnd = ts
        while (pathEnd < te && b[pathEnd] != '?'.code.toByte()) pathEnd++
        val http10 = p + 8 < rl && b[p + 6] == '1'.code.toByte() && b[p + 8] == '0'.code.toByte()
        p = rl + 1

        // Headers we care about.
        var cl = 0
        var authS = -1; var authE = -1
        var close = http10
        var he: Int
        while (true) {
            if (p + 1 >= e) return if (e - s > 16 * 1024) -1 else 0
            if (b[p] == '\r'.code.toByte() && b[p + 1] == '\n'.code.toByte()) { he = p + 2; break }
            var le = p
            while (le < e && b[le] != '\n'.code.toByte()) le++
            if (le >= e) return if (e - s > 16 * 1024) -1 else 0
            val f = b[p].toInt() or 0x20
            if (f == 'c'.code || f == 'a'.code || f == 't'.code) {
                val end = if (b[le - 1] == '\r'.code.toByte()) le - 1 else le
                var colon = p
                while (colon < end && b[colon] != ':'.code.toByte()) colon++
                var vs = colon + 1
                while (vs < end && (b[vs] == ' '.code.toByte() || b[vs] == '\t'.code.toByte())) vs++
                var ve = end
                while (ve > vs && (b[ve - 1] == ' '.code.toByte() || b[ve - 1] == '\t'.code.toByte())) ve--
                when (colon - p) {
                    14 -> if (ieq(b, p, "content-length")) {
                        cl = 0
                        for (k in vs until ve) {
                            val d = b[k] - '0'.code.toByte()
                            if (d !in 0..9) return -1
                            cl = cl * 10 + d
                            if (cl > MAX_REQ) return -1
                        }
                    }
                    13 -> if (ieq(b, p, "authorization")) { authS = vs; authE = ve }
                    10 -> if (ieq(b, p, "connection")) {
                        if (icontains(b, vs, ve, "close")) close = true
                        else if (icontains(b, vs, ve, "keep-alive")) close = false
                    }
                    17 -> if (ieq(b, p, "transfer-encoding")) return -1 // chunked bodies aren't supported
                }
            }
            p = le + 1
        }
        val total = he - s + cl
        if (cl > MAX_REQ - (he - s)) return -1
        if (e - s < total) return 0

        try {
            route(c, method, ts, pathEnd, authS, authE, he, he + cl, close)
        } catch (ex: Exception) {
            System.err.println("request failed: $ex")
            respond(c, H500, E_500, close)
        }
        return total
    }

    private fun route(c: Conn, method: Int, ps: Int, pe: Int, authS: Int, authE: Int, bs: Int, be: Int, close: Boolean) {
        val b = rbuf
        if (eq(b, ps, pe, P_FEED)) {
            if (method == GET) return respond(c, H200, db.feed(), close)
        } else if (eq(b, ps, pe, P_HEALTH)) {
            if (method == GET) return health(c, close)
        } else if (startsWith(b, ps, pe, P_POSTS)) {
            val rest = ps + P_POSTS.size
            if (rest == pe) {
                if (method == POST) return createPost(c, authS, authE, bs, be, close)
            } else if (b[rest] == '/'.code.toByte()) {
                val idS = rest + 1
                var slash = idS
                while (slash < pe && b[slash] != '/'.code.toByte()) slash++
                if (slash == pe) {
                    if (method == GET) {
                        val id = parseId(b, idS, pe)
                        if (id <= 0) return respond(c, H400, E_BAD_ID, close)
                        val post = db.post(id) ?: return respond(c, H404, E_NO_POST, close)
                        return respond(c, H200, post, close)
                    }
                } else if (eq(b, slash, pe, P_LIKE) && method == POST) {
                    if (!auth(c, authS, authE, close)) return
                    val id = parseId(b, idS, slash)
                    if (id <= 0) return respond(c, H400, E_BAD_ID, close)
                    return enqueue(WriteOp(c, true, jwt.userId, id, null, null, close))
                }
            }
        }
        respond(c, H404, E_NOT_FOUND, close)
    }

    private fun health(c: Conn, close: Boolean) {
        try {
            db.ping()
            val up = (System.nanoTime() - started) / 1_000_000_000L
            respond(c, H200, a("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":$up}"), close)
        } catch (e: Exception) {
            val msg = (e.message ?: e.toString()).replace("\\", "\\\\").replace("\"", "\\\"").replace(Regex("[\\x00-\\x1f]"), " ")
            respond(c, H503, a("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"$msg\"}"), close)
        }
    }

    private fun auth(c: Conn, s: Int, e: Int, close: Boolean): Boolean {
        when (jwt.verify(rbuf, s, e, System.currentTimeMillis() / 1000)) {
            Jwt.OK -> return true
            Jwt.MISSING -> respond(c, H401, E_MISSING, close)
            Jwt.INVALID -> respond(c, H401, E_INVALID, close)
            else -> respond(c, H401, E_PAYLOAD, close)
        }
        return false
    }

    private fun createPost(c: Conn, authS: Int, authE: Int, bs: Int, be: Int, close: Boolean) {
        if (!auth(c, authS, authE, close)) return
        val doc = try { Json.parse(rbuf, bs, be - bs) } catch (_: Json.Bad) { return respond(c, H400, E_JSON, close) }
        val raw = (doc as? Map<*, *>)?.get("body") as? String ?: return respond(c, H400, E_BODY, close)
        val body = raw.trim()
        if (body.isEmpty()) return respond(c, H400, E_BODY, close)
        if (body.codePointCount(0, body.length) > 500) return respond(c, H400, E_LONG, close)
        enqueue(WriteOp(c, false, jwt.userId, 0, body, jwt.username, close))
    }

    private fun enqueue(op: WriteOp) {
        if (writes.isEmpty()) firstWriteNs = System.nanoTime()
        op.conn.busy = true
        writes.add(op)
    }

    // ---------------------------------------------------------------- group commit

    private fun runBatch() {
        // Never block the event loop on another worker's write lock: if BEGIN is busy, keep the queued writes
        // (new ones keep joining the batch) and try again on the next pass. Give up after 5 s.
        var ok: Boolean
        try {
            if (!db.tryBegin()) {
                if (busySince == 0L) busySince = System.nanoTime()
                if (System.nanoTime() - busySince < 5_000_000_000L) { lockBusy = true; return }
                throw java.sql.SQLException("write lock busy for 5 s")
            }
            ok = true
        } catch (e: Exception) {
            System.err.println("group commit failed: $e")
            ok = false
        }
        lockBusy = false
        busySince = 0L
        val ops = writes
        writes = spare
        val n = ops.size
        val status = arrayOfNulls<ByteArray>(n)
        val bodies = arrayOfNulls<ByteArray>(n)
        if (ok) try {
            for (k in 0 until n) {
                val op = ops[k]
                try {
                    if (op.isLike) {
                        when (db.like(op.userId, op.postId)) {
                            -1 -> { status[k] = H404; bodies[k] = E_NO_POST }
                            1 -> { status[k] = H201; bodies[k] = likeBody(LIKED_NEW, op.postId) }
                            else -> { status[k] = H200; bodies[k] = likeBody(LIKED_OLD, op.postId) }
                        }
                    } else {
                        bodies[k] = db.insertPost(op.userId, op.body!!, op.author!!)
                        status[k] = H201
                    }
                } catch (e: Exception) {
                    System.err.println("write failed: $e")
                    status[k] = H500; bodies[k] = E_500
                }
            }
            db.commit()
        } catch (e: Exception) {
            System.err.println("group commit failed: $e")
            db.rollback()
            ok = false
        }
        // Committed (or failed): now answer everybody, and resume their pipelined input.
        for (k in 0 until n) {
            val op = ops[k]
            val c = op.conn
            c.busy = false
            if (c.closed) continue
            if (ok) respond(c, status[k]!!, bodies[k]!!, op.close) else respond(c, H500, E_500, op.close)
            if (c.pending != null) process(c, takePending(c)) else flush(c)
        }
        ops.clear()
        spare = ops
    }

    private fun likeBody(prefix: ByteArray, id: Long): ByteArray {
        var v = id; var p = num.size
        do { num[--p] = ('0'.code + (v % 10).toInt()).toByte(); v /= 10 } while (v > 0)
        val out = ByteArray(prefix.size + num.size - p + 1)
        System.arraycopy(prefix, 0, out, 0, prefix.size)
        System.arraycopy(num, p, out, prefix.size, num.size - p)
        out[out.size - 1] = '}'.code.toByte()
        return out
    }

    // ---------------------------------------------------------------- output

    private fun respond(c: Conn, head: ByteArray, body: ByteArray, close: Boolean) {
        val need = head.size + 12 + CLOSE.size + body.size
        if (obuf.remaining() < need) flush(c)
        if (obuf.remaining() < need) { // larger than the whole buffer: queue directly
            val bb = ByteBuffer.allocate(need)
            putResponse(bb, head, body, close); bb.flip()
            appendOut(c, bb)
        } else {
            putResponse(obuf, head, body, close)
        }
        if (close) c.closeAfter = true
    }

    private fun putResponse(bb: ByteBuffer, head: ByteArray, body: ByteArray, close: Boolean) {
        bb.put(head)
        var v = body.size; var p = num.size
        do { num[--p] = ('0'.code + v % 10).toByte(); v /= 10 } while (v > 0)
        bb.put(num, p, num.size - p)
        bb.put(if (close) CLOSE else CRLF2)
        bb.put(body)
    }

    /** Sends whatever is in obuf (all of it belongs to c). */
    private fun flush(c: Conn) {
        obuf.flip()
        if (obuf.hasRemaining() && !c.closed) {
            if (c.out == null) {
                try { c.ch.write(obuf) } catch (e: IOException) { obuf.clear(); close(c); return }
            }
            if (obuf.hasRemaining()) appendOut(c, obuf)
        }
        obuf.clear()
        if (c.closeAfter && c.out == null) close(c)
    }

    private fun appendOut(c: Conn, src: ByteBuffer) {
        val old = c.out
        val nb = ByteBuffer.allocate((old?.remaining() ?: 0) + src.remaining())
        if (old != null) nb.put(old)
        nb.put(src); nb.flip()
        c.out = nb
        if (c.key.isValid) c.key.interestOps(SelectionKey.OP_READ or SelectionKey.OP_WRITE)
    }

    private fun onWritable(c: Conn) {
        val o = c.out
        if (o == null) { c.key.interestOps(SelectionKey.OP_READ); return }
        try { c.ch.write(o) } catch (e: IOException) { close(c); return }
        if (!o.hasRemaining()) {
            c.out = null
            c.key.interestOps(SelectionKey.OP_READ)
            if (c.closeAfter) close(c)
        }
    }

    // ---------------------------------------------------------------- byte helpers

    private fun eq(b: ByteArray, s: Int, e: Int, lit: ByteArray): Boolean {
        if (e - s != lit.size) return false
        for (k in lit.indices) if (b[s + k] != lit[k]) return false
        return true
    }

    private fun startsWith(b: ByteArray, s: Int, e: Int, lit: ByteArray): Boolean {
        if (e - s < lit.size) return false
        for (k in lit.indices) if (b[s + k] != lit[k]) return false
        return true
    }

    /** Case-insensitive match of an ASCII lowercase literal at b[s..]. */
    private fun ieq(b: ByteArray, s: Int, lit: String): Boolean {
        for (k in lit.indices) if ((b[s + k].toInt() or 0x20) != lit[k].code) return false
        return true
    }

    private fun icontains(b: ByteArray, s: Int, e: Int, lit: String): Boolean {
        var i = s
        while (i + lit.length <= e) { if (ieq(b, i, lit)) return true; i++ }
        return false
    }

    /** Positive decimal integer, or -1. */
    private fun parseId(b: ByteArray, s: Int, e: Int): Long {
        if (s >= e || e - s > 18) return -1
        var v = 0L
        for (k in s until e) {
            val d = b[k] - '0'.code.toByte()
            if (d !in 0..9) return -1
            v = v * 10 + d
        }
        return if (v > 0) v else -1
    }
}
