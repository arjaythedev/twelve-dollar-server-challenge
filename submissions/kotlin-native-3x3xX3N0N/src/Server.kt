@file:OptIn(ExperimentalForeignApi::class)

package app

import kotlinx.cinterop.*
import knc.*
import platform.posix.memcpy
import platform.posix.memmove

class WriteOp(
    val slot: Int, val gen: Int, val isLike: Boolean, val userId: Long, val postId: Long,
    val body: ByteArray?, val author: ByteArray?, val close: Boolean,
)

/**
 * One thread, one event loop over io_uring (csrc/uring.c does the kernel plumbing). The HTTP parsing, routing and
 * handlers are the JVM entry's (kotlin-nio-3x3xX3N0N, Server.kt), ported: reads are answered inline from SQLite
 * (B-tree paths with SQL as the fallback, csrc/db.c); writes are queued and committed together (group commit) once
 * the oldest has waited WRITE_WINDOW_NS or BATCH_MAX are queued, and are answered only after COMMIT.
 * Connections are slots (ring << 16 | direct-descriptor index) with flat per-slot arrays: no object per connection.
 */
class Server(private val jwt: Jwt) {
    private val evs = nativeHeap.allocArray<kn_ev>(MAX_EV)
    // Request bytes are copied into rbuf (pinned once, for good) and parsed there; responses are built in obuf.
    private val rbuf = ByteArray(64 * 1024)
    private val rptr = rbuf.pin().addressOf(0)
    private val obuf = ByteArray(1 shl 20)
    private val optr = obuf.pin().addressOf(0)
    private val tmp = ByteArray(1 shl 16)
    private val tptr = tmp.pin().addressOf(0)
    private var olen = 0

    private val open = BooleanArray(NSLOT)
    private val sgen = IntArray(NSLOT)
    private val busy = BooleanArray(NSLOT)        // has a write waiting for the group commit
    private val closeAfter = BooleanArray(NSLOT)
    private val pending = arrayOfNulls<ByteArray>(NSLOT)   // unparsed input (partial or pipelined requests)
    private val pendingLen = IntArray(NSLOT)

    private var writes = ArrayList<WriteOp>(1024)
    private var spare = ArrayList<WriteOp>(1024)
    private val started = kn_now_ns()
    private var inFlight = ArrayList<WriteOp>(1024)   // the batch the committer thread is working on
    private var firstWriteNs = 0L
    private val num = ByteArray(24)
    private var tPoll = 0L; private var tRead = 0L; private var tBatch = 0L; private var tAll = 0L
    private var nReads = 0L; private var nPasses = 0L

    companion object {
        private const val MAX_EV = 4096
        // Events handled per pass (bounded pass): the rest stay in the completion queue for the next pass.
        private val PASS_EV = envLong("KN_MAX_EV", MAX_EV.toLong()).toInt().coerceIn(64, MAX_EV)
        private const val NSLOT = 8 shl 16
        private const val GET = 1
        private const val POST = 2
        private const val MAX_REQ = 60 * 1024
        private const val HEAD_ROOM = 160    // room left for the status line + headers before a body from SQLite

        // Group-commit window: a batch closes once its oldest write has waited 20 ms (or 1,024 are queued). Reads never
        // wait for it. Measured 160k users (LOCAL): committer thread 1.4-1.7 -> 0.8 us/req against 2 ms / 64, p95 2.3 ->
        // 8.4 ms (the limit is 500 ms); 10 ms and 40 ms were no better for CPU. Idea: go-iouring-DblD, fsharp-uring-delneg.
        private val WRITE_WINDOW_NS = envLong("KN_WINDOW_US", 20000) * 1000
        private val BATCH_MAX = envLong("KN_BATCH_MAX", 1024).toInt()
        // Batch wake (idea: kotlin-trikeshed-jnorthrup, rust-iouring-kensac): after a pass with fewer than PACE_NR
        // completions the next io_uring_enter waits on ring 0 alone for PACE_NR completions or PACE_US (or the
        // group-commit deadline); the secondary rings are collected after that wait (csrc/uring.c). Measured 160k users
        // (LOCAL, netns, two pairs): loop passes 8.2k -> 1.9k per second, 15.7 -> 14.7 and 16.7 -> 14.8 us/req, reads
        // p95 0.3 -> 2.2 ms. KN_PACE_NR=1 = the old loop (return at the first completion).
        private val PACE_NR = envLong("KN_PACE_NR", 256).toInt()   // after a light pass: wait for this many completions
        private val PACE_US = envLong("KN_PACE_US", 1000)          // ... or this long
        // KN_PASS=sweep (csrc/uring.c, after java-iouring-davidhsv): every pass that follows a non-empty one waits on
        // ring 0 for PACE_NR completions or PACE_US, not only after a light pass (KN_PACE_ALWAYS overrides).
        private val SWEEP = platform.posix.getenv("KN_PASS")?.toKString() == "sweep"
        private val PACE_ALWAYS = envLong("KN_PACE_ALWAYS", if (SWEEP) 1 else 0) != 0L
        private val STATS = envLong("KN_STATS", 0) != 0L
        private val HDR_STRUCT = envLong("KN_HDR", 1) != 0L   // one-pass head parser (request1); 0 = request0
        private val PROF = envLong("KN_PROF", 0) != 0L
        private val STALLS = envLong("KN_STALLS", 0) != 0L  // log passes whose work took > 100 ms   // time split on the stats line (two clock reads per stage)

        private fun a(s: String) = s.encodeToByteArray()
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
        var light = false
        var lastN = 0
        var lastStats = kn_now_ns()
        while (true) {
            // After a light pass the next wait gathers up to PACE_NR completions (or PACE_US, or until the group-commit
            // window ends), so one io_uring_enter serves more sockets; after a heavy pass it returns at the first one.
            val pace = light || (PACE_ALWAYS && lastN > 0)
            var waitNr = if (pace) PACE_NR else 1
            var timeoutUs = if (pace) PACE_US else -1L
            when {
                writes.isNotEmpty() && inFlight.isEmpty() -> {
                    val left = WRITE_WINDOW_NS - (kn_now_ns() - firstWriteNs)
                    val w = if (left <= 0 || writes.size >= BATCH_MAX) 0 else (left + 999) / 1000
                    timeoutUs = if (timeoutUs < 0) w else minOf(timeoutUs, w)
                }
            }
            val t0 = kn_now_ns()
            val n = ur_poll(evs, PASS_EV, waitNr, timeoutUs)
            val t1 = kn_now_ns()
            for (i in 0 until n) {
                val e = evs[i]
                when (e.kind) {
                    EV_DATA -> onData(e.slot, e.gen, e.ptr!!, e.len)
                    EV_OPEN -> { open[e.slot] = true; sgen[e.slot] = e.gen; busy[e.slot] = false; closeAfter[e.slot] = false; pending[e.slot] = null }
                    EV_CLOSED -> if (sgen[e.slot] == e.gen) { open[e.slot] = false; pending[e.slot] = null }
                    EV_WAKE -> finishBatch()
                }
            }
            ur_done()
            val t2 = kn_now_ns()
            val nw = writes.size
            if (writes.isNotEmpty() && inFlight.isEmpty() && (writes.size >= BATCH_MAX || kn_now_ns() - firstWriteNs >= WRITE_WINDOW_NS)) startBatch()
            if (db_pass_end() == 1) finishBatch()   // an on-loop commit that had to wait for the write lock is done
            if (STALLS) {
                // A pass whose own work (not the wait for completions) took > 100 ms: print where the time went.
                val t3 = kn_now_ns()
                if (t3 - t1 > 100_000_000L || (t1 - t0 > 100_000_000L && n > 0))
                    println("stall: poll ${(t1 - t0) / 1000} us (timeout $timeoutUs, $n events), handlers ${(t2 - t1) / 1000} us, " +
                        "batch ${(t3 - t2) / 1000} us ($nw writes, ${inFlight.size} in flight)")
            }
            if (PROF) {
                // ur_poll's time includes sleeping in io_uring_enter, so the busy part is estimated from CPU time below
                val t3 = kn_now_ns(); tPoll += t1 - t0; tBatch += t3 - t2; tAll += t3 - t0; nPasses++
            }
            // A light pass (few completions, nothing queued) makes the next wait gather more completions per syscall;
            // under load every pass is heavy and this never waits. An idle server (n == 0) sleeps without a timeout.
            light = n in 1 until PACE_NR
            lastN = n
            if (STATS && kn_now_ns() - lastStats > 10_000_000_000L) {
                lastStats = kn_now_ns()
                println("stats: open=${ur_stat(0)} rings=${ur_stat(3)} enobufs=${ur_stat(1)} rearms=${ur_stat(4)} sweeps=${ur_stat(5)} " +
                    "accept_err=${ur_stat(2)} btree=${db_stat(0)} sql=${db_stat(1)} snapshots=${db_stat(2)} parked=${db_stat(3)} walw=${db_stat(4)} walpw=${db_stat(5)} " +
                    "commits loop=${db_stat(6)} thread=${db_stat(7)} busy=${db_stat(8)} slow=${db_stat(9)} ckslow=${db_stat(10)} memslow=${db_stat(11)}" +
                    if (PROF) " | ms: poll+wait=${tPoll / 1000000} dbread=${tRead / 1000000} (${nReads} reads) " +
                        "batch=${tBatch / 1000000} loop=${tAll / 1000000} passes=$nPasses" else "")
                for (r in 0 until 8) if (ur_stat(100 + r) < 0) break
                tPoll = 0; tRead = 0; tBatch = 0; tAll = 0; nReads = 0; nPasses = 0
            }
        }
    }

    // ---------------------------------------------------------------- input

    private fun onData(slot: Int, gen: Int, p: CPointer<UByteVar>, n: Int) {
        if (!open[slot] || sgen[slot] != gen || closeAfter[slot]) return
        var len = 0
        val pend = pending[slot]
        if (pend != null) {
            len = pendingLen[slot]
            pend.copyInto(rbuf, 0, 0, len)
            pending[slot] = null
        }
        if (len + n > rbuf.size) { ur_close_after(slot, gen); closeAfter[slot] = true; return }
        memcpy(rptr + len, p, n.convert())
        len += n
        if (busy[slot]) { stash(slot, 0, len); return }   // a write is in the group commit: keep the order
        process(slot, gen, len)
    }

    private fun stash(slot: Int, from: Int, to: Int) {
        val rest = to - from
        val p = ByteArray(maxOf(rest, 1024))
        rbuf.copyInto(p, 0, from, to)
        pending[slot] = p; pendingLen[slot] = rest
    }

    /** Handles every complete request in rbuf[0, len), sends the answers, and stashes the remainder. */
    private fun process(slot: Int, gen: Int, len: Int) {
        var pos = 0
        while (pos < len && !busy[slot] && !closeAfter[slot]) {
            val used = request(slot, gen, pos, len)
            if (used < 0) { flush(slot, gen); ur_close_after(slot, gen); closeAfter[slot] = true; return }
            if (used == 0) break
            pos += used
        }
        flush(slot, gen)
        if (closeAfter[slot]) return
        if (pos < len) {
            if (len - pos >= MAX_REQ) { ur_close_after(slot, gen); closeAfter[slot] = true; return }
            stash(slot, pos, len)
        }
    }

    /** Parses one request at rbuf[s, e). Returns bytes consumed, 0 if incomplete, -1 to drop the connection. */
    private fun request(slot: Int, gen: Int, s: Int, e: Int): Int = if (HDR_STRUCT) request1(slot, gen, s, e) else request0(slot, gen, s, e)

    /** The original two-pass parser (KN_HDR=0): find the blank line, then split every header at its colon. */
    private fun request0(slot: Int, gen: Int, s: Int, e: Int): Int {
        val b = rbuf
        // End of headers.
        var he = -1
        var i = s
        while (i + 3 < e) {
            if (b[i + 3] != '\n'.code.toByte()) { i++; continue }
            if (b[i] == '\r'.code.toByte() && b[i + 1] == '\n'.code.toByte() && b[i + 2] == '\r'.code.toByte()) { he = i + 4; break }
            i++
        }
        if (he < 0) return if (e - s > 16 * 1024) -1 else 0

        // Request line.
        var method = 0
        var p: Int
        if (e - s > 4 && b[s] == 'G'.code.toByte() && b[s + 1] == 'E'.code.toByte() && b[s + 2] == 'T'.code.toByte() && b[s + 3] == ' '.code.toByte()) {
            method = GET; p = s + 4
        } else if (e - s > 5 && b[s] == 'P'.code.toByte() && b[s + 1] == 'O'.code.toByte() && b[s + 2] == 'S'.code.toByte() && b[s + 3] == 'T'.code.toByte() && b[s + 4] == ' '.code.toByte()) {
            method = POST; p = s + 5
        } else {
            p = s
            while (p < he && b[p] != ' '.code.toByte()) p++
            p++
        }
        val ts = p
        while (p < he && b[p] != ' '.code.toByte() && b[p] != '\r'.code.toByte()) p++
        val te = p
        var pathEnd = ts
        while (pathEnd < te && b[pathEnd] != '?'.code.toByte()) pathEnd++
        val http10 = p + 8 < he && b[p + 6] == '1'.code.toByte() && b[p + 8] == '0'.code.toByte()
        while (p < he && b[p] != '\n'.code.toByte()) p++
        p++

        // Headers we care about.
        var cl = 0
        var authS = -1; var authE = -1
        var close = http10
        while (p < he - 2) {
            var le = p
            while (b[le] != '\r'.code.toByte()) le++
            var colon = p
            while (colon < le && b[colon] != ':'.code.toByte()) colon++
            var vs = colon + 1
            while (vs < le && (b[vs] == ' '.code.toByte() || b[vs] == '\t'.code.toByte())) vs++
            var ve = le
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
            p = le + 2
        }
        val total = he - s + cl
        if (cl > MAX_REQ - (he - s)) return -1
        if (e - s < total) return 0

        try {
            route(slot, gen, method, ts, pathEnd, authS, authE, he, he + cl, close)
        } catch (ex: Exception) {
            println("request failed: $ex")
            respond(slot, gen, H500, E_500, close)
        }
        return total
    }

    /**
     * One pass over the head (idea: kotlin-trikeshed-jnorthrup's "parse the headers as a struct, first character,
     * then verify"): each line is scanned once for its CR; a line is decoded only if its first character and the
     * colon at the fixed offset of its name say it is one of the headers we use (Content-Length, Authorization,
     * Connection, Transfer-Encoding); every other line costs the CR scan alone. Values are offsets into rbuf.
     * Same results as request0 (any case of the names, optional spaces/tabs around values).
     */
    private fun request1(slot: Int, gen: Int, s: Int, e: Int): Int {
        val b = rbuf
        val CR = '\r'.code.toByte()
        val SP = ' '.code.toByte()
        // Request line: method by its first byte, then the target up to the next space.
        var method = 0
        var p: Int
        if (e - s > 4 && b[s] == 'G'.code.toByte() && b[s + 1] == 'E'.code.toByte() && b[s + 2] == 'T'.code.toByte() && b[s + 3] == SP) {
            method = GET; p = s + 4
        } else if (e - s > 5 && b[s] == 'P'.code.toByte() && b[s + 1] == 'O'.code.toByte() && b[s + 2] == 'S'.code.toByte() && b[s + 3] == 'T'.code.toByte() && b[s + 4] == SP) {
            method = POST; p = s + 5
        } else {
            p = s
            while (p < e && b[p] != SP && b[p] != CR) p++
            p++
        }
        val ts = p
        var pathEnd = -1
        while (p < e) {
            val c = b[p]
            if (c == SP || c == CR) break
            if (c == '?'.code.toByte() && pathEnd < 0) pathEnd = p
            p++
        }
        val te = p
        if (pathEnd < 0) pathEnd = te
        while (p < e && b[p] != CR) p++
        if (p + 1 >= e) return if (e - s > 16 * 1024) -1 else 0
        // "HTTP/1.0" ends the line: its '0' is right before the CR
        val http10 = p - te >= 9 && b[p - 1] == '0'.code.toByte() && b[p - 3] == '1'.code.toByte() && b[p - 4] == '/'.code.toByte()
        p += 2

        var cl = 0
        var authS = -1; var authE = -1
        var close = http10
        var he: Int
        while (true) {
            if (p + 1 >= e) return if (e - s > 16 * 1024) -1 else 0
            if (b[p] == CR) { he = p + 2; break }               // the blank line
            var le = p
            while (le < e && b[le] != CR) le++
            if (le + 1 >= e) return if (e - s > 16 * 1024) -1 else 0
            val n = le - p
            when (b[p].toInt() or 0x20) {
                'c'.code -> if (n > 14 && b[p + 14] == ':'.code.toByte() && ieq(b, p, "content-length")) {
                    var vs = p + 15; var ve = le
                    while (vs < ve && (b[vs] == SP || b[vs] == '\t'.code.toByte())) vs++
                    while (ve > vs && (b[ve - 1] == SP || b[ve - 1] == '\t'.code.toByte())) ve--
                    cl = 0
                    for (k in vs until ve) {
                        val d = b[k] - '0'.code.toByte()
                        if (d !in 0..9) return -1
                        cl = cl * 10 + d
                        if (cl > MAX_REQ) return -1
                    }
                } else if (n > 10 && b[p + 10] == ':'.code.toByte() && ieq(b, p, "connection")) {
                    if (icontains(b, p + 11, le, "close")) close = true
                    else if (icontains(b, p + 11, le, "keep-alive")) close = false
                }
                'a'.code -> if (n > 13 && b[p + 13] == ':'.code.toByte() && ieq(b, p, "authorization")) {
                    var vs = p + 14; var ve = le
                    while (vs < ve && (b[vs] == SP || b[vs] == '\t'.code.toByte())) vs++
                    while (ve > vs && (b[ve - 1] == SP || b[ve - 1] == '\t'.code.toByte())) ve--
                    authS = vs; authE = ve
                }
                't'.code -> if (n > 17 && b[p + 17] == ':'.code.toByte() && ieq(b, p, "transfer-encoding")) return -1
            }
            p = le + 2
        }
        val total = he - s + cl
        if (cl > MAX_REQ - (he - s)) return -1
        if (e - s < total) return 0
        try {
            route(slot, gen, method, ts, pathEnd, authS, authE, he, he + cl, close)
        } catch (ex: Exception) {
            println("request failed: $ex")
            respond(slot, gen, H500, E_500, close)
        }
        return total
    }

    private fun route(slot: Int, gen: Int, method: Int, ps: Int, pe: Int, authS: Int, authE: Int, bs: Int, be: Int, close: Boolean) {
        val b = rbuf
        if (eq(b, ps, pe, P_FEED)) {
            if (method == GET) return fromDb(slot, gen, close, -1L)
        } else if (eq(b, ps, pe, P_HEALTH)) {
            if (method == GET) return health(slot, gen, close)
        } else if (startsWith(b, ps, pe, P_POSTS)) {
            val rest = ps + P_POSTS.size
            if (rest == pe) {
                if (method == POST) return createPost(slot, gen, authS, authE, bs, be, close)
            } else if (b[rest] == '/'.code.toByte()) {
                val idS = rest + 1
                var sl = idS
                while (sl < pe && b[sl] != '/'.code.toByte()) sl++
                if (sl == pe) {
                    if (method == GET) {
                        val id = parseId(b, idS, pe)
                        if (id <= 0) return respond(slot, gen, H400, E_BAD_ID, close)
                        return fromDb(slot, gen, close, id)
                    }
                } else if (eq(b, sl, pe, P_LIKE) && method == POST) {
                    if (!auth(slot, gen, authS, authE, close)) return
                    val id = parseId(b, idS, sl)
                    if (id <= 0) return respond(slot, gen, H400, E_BAD_ID, close)
                    return enqueue(WriteOp(slot, gen, true, jwt.userId, id, null, null, close))
                }
            }
        }
        respond(slot, gen, H404, E_NOT_FOUND, close)
    }

    /** GET /feed (id < 0) or GET /posts/:id: SQLite writes the JSON body straight into obuf, after room for the head. */
    private fun fromDb(slot: Int, gen: Int, close: Boolean, id: Long) {
        if (obuf.size - olen < HEAD_ROOM + 64 * 1024) flush(slot, gen)
        val at = olen + HEAD_ROOM
        val out = (optr + at)!!.reinterpret<UByteVar>()
        val r0 = if (PROF) kn_now_ns() else 0L
        val n = if (id < 0) db_feed(out, obuf.size - at) else db_post(id, out, obuf.size - at)
        if (PROF) { tRead += kn_now_ns() - r0; nReads++ }
        if (n < 0) return respond(slot, gen, H500, E_500, close)
        if (n == 0) return respond(slot, gen, H404, E_NO_POST, close)
        val h = putHead(olen, H200, n, close)
        memmove(optr + olen + h, optr + at, n.convert())
        olen += h + n
        if (close) closeAfter[slot] = true
    }

    private fun health(slot: Int, gen: Int, close: Boolean) {
        val err = tptr
        if (db_health(err, 512) == 0) {
            val up = (kn_now_ns() - started) / 1_000_000_000L
            respond(slot, gen, H200, a("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":$up}"), close)
        } else {
            val msg = err.toKString().replace("\\", "\\\\").replace("\"", "\\\"").replace(Regex("[\\x00-\\x1f]"), " ")
            respond(slot, gen, H503, a("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"$msg\"}"), close)
        }
    }

    private fun auth(slot: Int, gen: Int, s: Int, e: Int, close: Boolean): Boolean {
        when (jwt.verify(rbuf, s, e, kn_wall_s())) {
            Jwt.OK -> return true
            Jwt.MISSING -> respond(slot, gen, H401, E_MISSING, close)
            Jwt.INVALID -> respond(slot, gen, H401, E_INVALID, close)
            else -> respond(slot, gen, H401, E_PAYLOAD, close)
        }
        return false
    }

    private fun createPost(slot: Int, gen: Int, authS: Int, authE: Int, bs: Int, be: Int, close: Boolean) {
        if (!auth(slot, gen, authS, authE, close)) return
        val doc = try { Json.parse(rbuf, bs, be - bs) } catch (_: Json.Bad) { return respond(slot, gen, H400, E_JSON, close) }
        val raw = (doc as? Map<*, *>)?.get("body") as? String ?: return respond(slot, gen, H400, E_BODY, close)
        val body = raw.trim()
        if (body.isEmpty()) return respond(slot, gen, H400, E_BODY, close)
        if (codePoints(body) > 500) return respond(slot, gen, H400, E_LONG, close)
        enqueue(WriteOp(slot, gen, false, jwt.userId, 0, body.encodeToByteArray(), jwt.username.encodeToByteArray(), close))
    }

    private fun codePoints(s: String): Int {
        var n = 0; var i = 0
        while (i < s.length) {
            if (s[i].isHighSurrogate() && i + 1 < s.length && s[i + 1].isLowSurrogate()) i++
            i++; n++
        }
        return n
    }

    private fun enqueue(op: WriteOp) {
        if (writes.isEmpty()) firstWriteNs = kn_now_ns()
        busy[op.slot] = true
        writes.add(op)
    }

    // ---------------------------------------------------------------- group commit

    /** Commits the queued writes (on the loop, or hands them to the committer thread: csrc/db.c), unless a batch is
     *  still being committed. */
    private fun startBatch() {
        if (inFlight.isNotEmpty()) return
        val ops = writes
        for (op in ops) {
            val rc = if (op.isLike) db_wq_add_like(op.userId, op.postId) else {
                val body = op.body!!; val author = op.author!!
                body.usePinned { bp -> author.usePinned { ap ->
                    db_wq_add_post(op.userId, bp.addressOf(0).reinterpret(), body.size, ap.addressOf(0).reinterpret(), author.size)
                } }
            }
            if (rc != 0) println("group commit: out of memory staging a write")
        }
        writes = inFlight
        inFlight = ops
        if (db_wq_start() == 2) finishBatch()   // committed on the loop: answer now
    }

    /** The batch is committed (on the loop, or the committer thread signalled): answer everybody and resume their
     *  pipelined input. */
    private fun finishBatch() {
        val n = db_wq_done()
        if (n < 0) return
        val ops = inFlight
        val out = tptr.reinterpret<UByteVar>()
        val code = nativeHeap.alloc<IntVar>()
        val status = arrayOfNulls<ByteArray>(ops.size)
        val bodies = arrayOfNulls<ByteArray>(ops.size)
        for (k in 0 until ops.size) {
            val op = ops[k]
            val len = if (k < n) db_wq_result(k, code.ptr, out, tmp.size) else { code.value = -2; 0 }
            when {
                op.isLike && code.value == -1 -> { status[k] = H404; bodies[k] = E_NO_POST }
                op.isLike && code.value == 1 -> { status[k] = H201; bodies[k] = likeBody(LIKED_NEW, op.postId) }
                op.isLike && code.value == 0 -> { status[k] = H200; bodies[k] = likeBody(LIKED_OLD, op.postId) }
                !op.isLike && code.value == 201 -> { status[k] = H201; bodies[k] = tmp.copyOf(len) }
                else -> { status[k] = H500; bodies[k] = E_500 }
            }
        }
        nativeHeap.free(code)
        db_wq_release()   // the reader drops its snapshot: later reads see this commit
        inFlight = spare
        spare = ops
        for (k in 0 until ops.size) {
            val op = ops[k]
            val slot = op.slot
            if (!open[slot] || sgen[slot] != op.gen) continue
            busy[slot] = false
            respond(slot, op.gen, status[k]!!, bodies[k]!!, op.close)
            val pend = pending[slot]
            if (pend != null && !closeAfter[slot]) {
                val len = pendingLen[slot]
                flush(slot, op.gen)
                pend.copyInto(rbuf, 0, 0, len)
                pending[slot] = null
                process(slot, op.gen, len)
            } else flush(slot, op.gen)
        }
        ops.clear()
    }

    private fun likeBody(prefix: ByteArray, id: Long): ByteArray {
        var v = id; var p = num.size
        do { num[--p] = ('0'.code + (v % 10).toInt()).toByte(); v /= 10 } while (v > 0)
        val out = ByteArray(prefix.size + num.size - p + 1)
        prefix.copyInto(out, 0)
        num.copyInto(out, prefix.size, p, num.size)
        out[out.size - 1] = '}'.code.toByte()
        return out
    }

    // ---------------------------------------------------------------- output

    /** Writes the status line + headers for a body of n bytes at obuf[at]; returns their length. */
    private fun putHead(at: Int, head: ByteArray, n: Int, close: Boolean): Int {
        var o = at
        head.copyInto(obuf, o); o += head.size
        var v = n; var p = num.size
        do { num[--p] = ('0'.code + v % 10).toByte(); v /= 10 } while (v > 0)
        num.copyInto(obuf, o, p, num.size); o += num.size - p
        val end = if (close) CLOSE else CRLF2
        end.copyInto(obuf, o); o += end.size
        return o - at
    }

    private fun respond(slot: Int, gen: Int, head: ByteArray, body: ByteArray, close: Boolean) {
        if (obuf.size - olen < HEAD_ROOM + body.size) flush(slot, gen)
        if (obuf.size - olen < HEAD_ROOM + body.size) { // larger than the whole buffer: send it on its own
            val h = putHead(0, head, body.size, close)
            ur_send(slot, gen, optr.reinterpret(), h)
            body.usePinned { ur_send(slot, gen, it.addressOf(0).reinterpret(), body.size) }
        } else {
            olen += putHead(olen, head, body.size, close)
            body.copyInto(obuf, olen); olen += body.size
        }
        if (close) closeAfter[slot] = true
    }

    /** Sends whatever is in obuf (all of it belongs to this connection), then closes if asked. */
    private fun flush(slot: Int, gen: Int) {
        if (olen > 0) ur_send(slot, gen, optr.reinterpret(), olen)
        olen = 0
        if (closeAfter[slot] && !busy[slot]) ur_close_after(slot, gen)
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

fun envLong(name: String, def: Long): Long = platform.posix.getenv(name)?.toKString()?.toLongOrNull() ?: def
