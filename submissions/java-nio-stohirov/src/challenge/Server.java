package challenge;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.VarHandle;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;

/**
 * The HTTP/1.1 server: one thread drives every io_uring and runs SQLite inline.
 *
 * A pass: submit what the last pass queued and collect completions, handle them (each request runs inside
 * the pass's one transaction and its response goes into an off-heap arena), commit, then queue one send per
 * connection. So nothing is sent before its commit, and a pass costs one system call per ring.
 */
final class Server {
    static final int LISTENERS = 8;
    private static final int MAX_RINGS = 16, BACKLOG = 4096;
    private static final int MAX_ACCEPTS_IN_FLIGHT = 512;
    // A pass waits on ring 0 for this many completions or this long, so bigger passes share one COMMIT.
    private static final int WAIT_MIN = 256;
    private static final long WAIT_NANOS = 1_000_000L, RETRY_RING_NANOS = 1_000_000_000L;

    private static final int READ_CAP = 64 * 1024;
    private static final int MAX_HEAD = 16 * 1024;
    private static final int MAX_BODY = 32 * 1024;
    private static final int OUT_CAP = 4 * 1024 * 1024;
    // Room one response may need. A feed's worst case: 20 posts of 500 four-byte or escaped characters.
    private static final int OUT_RESERVE = 512 * 1024;
    private static final int HOLD = -1;

    // Completion kinds: the top byte of user_data. A send's top bit says which arena it reads from.
    private static final int ACCEPT = 1, RECV = 2, SEND = 3, POLL = 4, SHUTDOWN = 5, CLOSE = 6;
    private static final VarHandle INT = ValueLayout.JAVA_INT.varHandle();

    private static final int NOT_FOUND = 0, HEALTH = 1, FEED = 2, GET_POST = 3, CREATE = 4, LIKE = 5;

    private static final byte[] E_NOT_FOUND = err("not found"), E_INVALID_ID = err("invalid post id"),
            E_NO_POST = err("post not found"), E_MISSING_TOKEN = err("missing bearer token"),
            E_INVALID_TOKEN = err("invalid or expired token"), E_BAD_PAYLOAD = err("invalid token payload"),
            E_MALFORMED = err("malformed JSON body"), E_BODY_REQUIRED = err("body is required"),
            E_BODY_TOO_LONG = err("body must be at most 500 characters"), E_INTERNAL = err("internal server error"),
            E_BAD_REQUEST = err("bad request"), E_TOO_LARGE = err("request too large"),
            E_NOT_IMPLEMENTED = err("not implemented");
    private static final byte[] LIKED_NEW = Out.ascii("{\"liked\":true,\"already_liked\":false,\"post_id\":");
    private static final byte[] LIKED_AGAIN = Out.ascii("{\"liked\":true,\"already_liked\":true,\"post_id\":");
    private static final byte[] HEALTH_OK = Out.ascii("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");

    private static byte[] err(String message) { return Out.ascii("{\"error\":\"" + message + "\"}"); }

    /** A connection. Its buffers are null unless it holds a partial request or an unsent response. */
    static final class Conn {
        final Ring ring;
        final int slot, gen;
        byte[] left;        // received bytes not yet answered
        byte[] pending;     // response bytes the socket didn't take yet, from pendingFrom on
        int pendingFrom;
        int stageFrom, stageTo, sendFrom, sendLen, sendArena;
        boolean staged, sending, parked, closeAfterWrite, eof, closing;

        Conn(Ring ring, int slot, int gen) {
            this.ring = ring;
            this.slot = slot;
            this.gen = gen;
        }

        /** Its next response can't be built yet, so what arrives waits in left. */
        boolean busy() { return sending || pending != null || parked || closing; }
    }

    private final Db db;
    private final Jwt jwt;
    private final int[] listenFds = new int[LISTENERS];
    private final long startNanos = System.nanoTime();

    private final int slots, margin, acceptsPerListener;
    private final Ring[] rings = new Ring[MAX_RINGS];
    private int ringCount;
    private Ring acceptRing;
    private long retryRingAt;
    private int nextGen;

    private final MemorySegment waitArg = Arena.global().allocate(24 + 16, 8);
    private final long waitArgAddr = waitArg.address();

    /**
     * Two arenas. Sends never outlive the enter that submits them (see Ring.send), but a short send's rest is
     * only learned in the next pass, so a pass writes to the arena the last pass didn't.
     */
    private final Out[] arenas = {new Out(OUT_CAP), new Out(OUT_CAP)};
    private final int[] sendsInFlight = new int[2];
    private int arena;
    private Out out = arenas[0];

    private final byte[] rb = new byte[READ_CAP + MAX_HEAD + MAX_BODY];
    private final ArrayList<Conn> staged = new ArrayList<>(), deferred = new ArrayList<>(), held = new ArrayList<>();
    private final ArrayList<Conn> retry = new ArrayList<>();

    Server(Db db, Jwt jwt, String host, int port, int slots) throws java.io.IOException {
        this.db = db;
        this.jwt = jwt;
        this.slots = slots;
        // Accepts in flight fit inside the margin, so a ring is never asked for a slot it doesn't have.
        int depth = Math.clamp(slots / 16, LISTENERS, MAX_ACCEPTS_IN_FLIGHT);
        acceptsPerListener = depth / LISTENERS;
        margin = Math.min(Math.clamp(slots / 8, depth + 1, 2 * MAX_ACCEPTS_IN_FLIGHT), slots / 4);
        InetAddress addr = InetAddress.getByName(host);
        for (int i = 0; i < LISTENERS; i++) listenFds[i] = Sys.listen(addr, port, BACKLOG);
        waitArg.set(JAVA_LONG, 16, waitArgAddr + 24); // ts
        waitArg.set(JAVA_LONG, 24, 0L);               // tv_sec
        waitArg.set(JAVA_LONG, 32, WAIT_NANOS);       // tv_nsec
        acceptRing = addRing();
    }

    private Ring addRing() {
        Ring r = new Ring(slots);
        rings[ringCount++] = r;
        return r;
    }

    void run() {
        fillAccepts();
        while (true) {
            enterRings();
            flipArena();
            for (int i = 0; i < ringCount; i++) drain(rings[i]);
            resumeParked();
            endPass();
        }
    }

    /** The last ring goes first and ring 0 waits, so no ring's sends sit behind the wait. */
    private void enterRings() {
        for (int i = ringCount - 1; i > 0; i--) rings[i].enter();
        rings[0].enterAndWait(deferred.isEmpty() ? WAIT_MIN : 0, waitArgAddr, 24);
    }

    private void flipArena() {
        int other = 1 - arena;
        if (sendsInFlight[other] == 0) {
            arena = other;
            out = arenas[other];
            out.pos = 0;
        } else if (sendsInFlight[arena] == 0) {
            out.pos = 0;
        }
    }

    private void drain(Ring r) {
        int head = r.cqHead;
        int tail = (int) INT.getAcquire(r.cq, r.cqTailOff);
        while (head != tail) {
            long off = r.cqesOff + (long) (head & r.cqMask) * 16;
            onCompletion(r, r.cq.get(JAVA_LONG, off), r.cq.get(JAVA_INT, off + 8), r.cq.get(JAVA_INT, off + 12));
            head++;
            // Handling these can submit (a close storm queues two entries per connection), and what the
            // kernel completes meanwhile needs room here, or it goes to a kernel-side overflow list.
            if ((head & 255) == 0) INT.setRelease(r.cq, r.cqHeadOff, head);
        }
        r.cqHead = head;
        INT.setRelease(r.cq, r.cqHeadOff, head);
        r.publishBufs();
    }

    private static long userData(int kind, int gen, int slot) {
        return (long) kind << 56 | (long) (gen & 0xffffff) << 32 | slot;
    }

    private void onCompletion(Ring r, long ud, int res, int flags) {
        int top = (int) (ud >>> 56);
        int kind = top & 0x7f;
        int slot = (int) ud;
        if (kind == ACCEPT) {
            onAccept(r, slot, res);
            return;
        }
        if (kind == SEND) sendsInFlight[top >>> 7]--;
        Conn c = r.conns[slot];
        boolean live = c != null && c.gen == ((int) (ud >>> 32) & 0xffffff);
        if (kind == RECV) {
            int bid = (flags & Ring.CQE_F_BUFFER) != 0 ? flags >>> 16 : -1;
            if (live && !c.closing) onRecv(r, c, res, bid, (flags & Ring.CQE_F_MORE) != 0);
            if (bid >= 0) r.recycle(bid);
            return;
        }
        if (!live) return;
        switch (kind) {
            case SEND -> { if (!c.closing) onSend(c, res, top >>> 7); }
            case POLL -> { if (!c.closing) flushPending(c); }
            case CLOSE -> {
                r.conns[slot] = null;
                r.open--;
                if (r.full && r.open + 2 * margin < slots) r.full = false;
            }
            default -> { }
        }
    }

    private void onAccept(Ring r, int listener, int res) {
        r.accepts[listener]--;
        if (res >= 0) {
            Conn c = new Conn(r, res, nextGen++ & 0xffffff);
            r.conns[res] = c;
            r.recv(res, userData(RECV, c.gen, res));
            if (++r.open + margin >= slots) r.full = true;
        } else if (res == -Sys.ENFILE) {
            r.full = true;
        }
    }

    private void onRecv(Ring r, Conn c, int res, int bid, boolean more) {
        if (res > 0) {
            if (bid >= 0) onData(r, c, bid, res);
            if (!more && !c.closing) rearm(r, c);
        } else if (res == -Sys.ENOBUFS) {
            rearm(r, c); // after this pass has returned its buffers
        } else if (res == 0) {
            c.eof = true;
            settle(c);
        } else {
            armClose(c);
        }
    }

    private static void rearm(Ring r, Conn c) {
        if (r.rearmCount == r.rearm.length) r.rearm = Arrays.copyOf(r.rearm, r.rearmCount * 2);
        r.rearm[r.rearmCount++] = c;
    }

    private void onData(Ring r, Conn c, int bid, int n) {
        int have = c.left == null ? 0 : c.left.length;
        if (have + n > rb.length) {
            armClose(c);
            return;
        }
        if (c.busy()) {
            byte[] l = have == 0 ? new byte[n] : Arrays.copyOf(c.left, have + n);
            MemorySegment.copy(r.bufs, JAVA_BYTE, r.bufOffset(bid), l, have, n);
            c.left = l;
            return;
        }
        if (have > 0) System.arraycopy(c.left, 0, rb, 0, have);
        MemorySegment.copy(r.bufs, JAVA_BYTE, r.bufOffset(bid), rb, have, n);
        c.left = null;
        process(c, have + n);
    }

    /** Runs what the connection has waiting if it can, and closes it if the peer is gone and nothing is owed. */
    private void settle(Conn c) {
        if (c.closing) return;
        if (c.left != null && !c.busy()) {
            byte[] l = c.left;
            System.arraycopy(l, 0, rb, 0, l.length);
            c.left = null;
            process(c, l.length);
        }
        if (c.eof && !c.busy() && !c.staged) armClose(c);
    }

    /** Answers the requests in rb[0, end), leaving what can't be answered yet in c.left. */
    private void process(Conn c, int end) {
        // A second batch in one pass must follow the first in the arena: one connection gets one send.
        if (c.staged && c.stageTo != out.pos) {
            c.left = Arrays.copyOf(rb, end);
            park(c, deferred);
            return;
        }
        int start = out.pos, p = 0;
        while (p < end) {
            if (out.free() < OUT_RESERVE) {
                park(c, deferred);
                break;
            }
            int used = request(c, rb, p, end);
            if (used == 0) break;
            if (used == HOLD) {
                park(c, held);
                break;
            }
            p += used;
            if (c.closeAfterWrite) {
                p = end;
                break;
            }
        }
        c.left = p < end ? Arrays.copyOfRange(rb, p, end) : null;
        if (out.pos > start) stage(c, start);
    }

    private void stage(Conn c, int start) {
        if (!c.staged) {
            c.staged = true;
            c.stageFrom = start;
            staged.add(c);
        }
        c.stageTo = out.pos;
    }

    private static void park(Conn c, ArrayList<Conn> list) {
        c.parked = true;
        list.add(c);
    }

    private void resumeParked() {
        if (!deferred.isEmpty()) {
            retry.clear();
            retry.addAll(deferred);
            deferred.clear();
            resume(retry);
        }
        if (!held.isEmpty() && !db.writesHeld()) {
            retry.clear();
            retry.addAll(held);
            held.clear();
            resume(retry);
        }
    }

    private void resume(ArrayList<Conn> list) {
        for (int i = 0, n = list.size(); i < n; i++) {
            Conn c = list.get(i);
            c.parked = false;
            if (c.pending != null && !c.sending) flushPending(c);
            settle(c);
        }
    }

    private void endPass() {
        boolean committed = db.commit();
        for (int i = 0, n = staged.size(); i < n; i++) {
            Conn c = staged.get(i);
            c.staged = false;
            if (c.closing) continue;
            if (committed) startSend(c);
            else armClose(c);
        }
        staged.clear();
        db.tick();
        for (int i = 0; i < ringCount; i++) {
            Ring r = rings[i];
            for (int k = 0; k < r.rearmCount; k++) {
                Conn c = r.rearm[k];
                if (!c.closing) r.recv(c.slot, userData(RECV, c.gen, c.slot));
                r.rearm[k] = null;
            }
            r.rearmCount = 0;
        }
        fillAccepts();
    }

    private void startSend(Conn c) {
        c.sending = true;
        c.sendArena = arena;
        c.sendFrom = c.stageFrom;
        c.sendLen = c.stageTo - c.stageFrom;
        sendsInFlight[arena]++;
        c.ring.send(c.slot, out.address(c.sendFrom), c.sendLen, userData(SEND | arena << 7, c.gen, c.slot));
    }

    private void onSend(Conn c, int res, int fromArena) {
        c.sending = false;
        if (res < 0 && res != -Sys.EAGAIN && res != -Sys.EINTR) {
            armClose(c);
            return;
        }
        int sent = Math.max(res, 0);
        if (c.pending != null) {
            c.pendingFrom += sent;
            if (c.pendingFrom < c.pending.length) {
                armPollOut(c);
                return;
            }
            c.pending = null;
            c.pendingFrom = 0;
        } else if (sent < c.sendLen) {
            // The socket buffer is full. The arena is still intact: this pass writes to the other one.
            c.pending = new byte[c.sendLen - sent];
            MemorySegment.copy(arenas[fromArena].seg, JAVA_BYTE, c.sendFrom + sent, c.pending, 0, c.pending.length);
            c.pendingFrom = 0;
            armPollOut(c);
            return;
        }
        if (c.closeAfterWrite) armClose(c);
        else settle(c);
    }

    private void armPollOut(Conn c) {
        c.ring.pollWritable(c.slot, userData(POLL, c.gen, c.slot));
    }

    /** The socket can take more: sends the next part of what it refused, through this pass's arena. */
    private void flushPending(Conn c) {
        int n = Math.min(c.pending.length - c.pendingFrom, out.free() - OUT_RESERVE);
        if (n <= 0 || c.staged && c.stageTo != out.pos) {
            if (!c.parked) park(c, deferred);
            return;
        }
        int start = out.pos;
        out.put(c.pending, c.pendingFrom, n);
        stage(c, start);
    }

    private void armClose(Conn c) {
        if (c.closing) return;
        c.closing = true;
        c.left = null;
        c.pending = null;
        c.ring.shutdownAndClose(c.slot, userData(SHUTDOWN, c.gen, c.slot), userData(CLOSE, c.gen, c.slot));
    }

    /** New connections go to one ring, the first with room. Accepts already in flight fit in its margin. */
    private void fillAccepts() {
        if (acceptRing.full && !moveAcceptRing()) return;
        Ring r = acceptRing;
        for (int li = 0; li < LISTENERS; li++) {
            while (r.accepts[li] < acceptsPerListener) {
                r.accept(listenFds[li], userData(ACCEPT, 0, li));
                r.accepts[li]++;
            }
        }
    }

    private boolean moveAcceptRing() {
        for (int i = 0; i < ringCount; i++) {
            if (!rings[i].full) {
                acceptRing = rings[i];
                return true;
            }
        }
        if (ringCount == MAX_RINGS || System.nanoTime() < retryRingAt) return false;
        try {
            acceptRing = addRing();
            return true;
        } catch (RuntimeException e) {
            System.err.println("can't add a ring: " + e.getMessage());
            retryRingAt = System.nanoTime() + RETRY_RING_NANOS;
            return false;
        }
    }

    private int pathFrom, pathTo, authFrom, authTo, bodyFrom, bodyLen;

    private int request(Conn c, byte[] b, int p, int end) {
        int he = headerEnd(b, p, end);
        if (he < 0) {
            if (end - p > MAX_HEAD) return reject(c, Out.S431, E_TOO_LARGE, end - p);
            return 0;
        }
        int sp1 = indexOf(b, (byte) ' ', p, he);
        int sp2 = sp1 < 0 ? -1 : indexOf(b, (byte) ' ', sp1 + 1, he);
        int eol = sp2 < 0 ? -1 : indexOf(b, (byte) '\r', sp2 + 1, he);
        if (eol < 0 || eol - sp2 - 1 != 8 || !startsWith(b, sp2 + 1, "HTTP/1.")) {
            return reject(c, Out.S400, E_BAD_REQUEST, end - p);
        }
        boolean http11 = b[sp2 + 8] == '1';
        boolean isGet = sp1 - p == 3 && b[p] == 'G' && b[p + 1] == 'E' && b[p + 2] == 'T';
        boolean isPost = sp1 - p == 4 && b[p] == 'P' && b[p + 1] == 'O' && b[p + 2] == 'S' && b[p + 3] == 'T';
        pathFrom = sp1 + 1;
        pathTo = sp2;
        int q = indexOf(b, (byte) '?', pathFrom, pathTo);
        if (q >= 0) pathTo = q;

        authFrom = authTo = -1;
        long contentLength = 0;
        boolean connClose = false, connKeepAlive = false, hasLength = false;
        int teFrom = -1, teTo = -1;
        int line = eol + 2;
        while (line < he - 2) {
            int le = indexOf(b, (byte) '\r', line, he);
            int colon = indexOf(b, (byte) ':', line, le);
            if (colon > line) {
                int vs = colon + 1, ve = le;
                while (vs < ve && (b[vs] == ' ' || b[vs] == '\t')) vs++;
                while (ve > vs && (b[ve - 1] == ' ' || b[ve - 1] == '\t')) ve--;
                int nl = colon - line;
                if (nl == 13 && nameIs(b, line, "authorization")) {
                    authFrom = vs;
                    authTo = ve;
                } else if (nl == 14 && nameIs(b, line, "content-length")) {
                    contentLength = 0;
                    hasLength = true;
                    if (vs == ve) return reject(c, Out.S400, E_BAD_REQUEST, end - p);
                    for (int k = vs; k < ve; k++) {
                        if (b[k] < '0' || b[k] > '9' || contentLength > MAX_BODY) {
                            return reject(c, b[k] < '0' || b[k] > '9' ? Out.S400 : Out.S413,
                                    b[k] < '0' || b[k] > '9' ? E_BAD_REQUEST : E_TOO_LARGE, end - p);
                        }
                        contentLength = contentLength * 10 + (b[k] - '0');
                    }
                } else if (nl == 10 && nameIs(b, line, "connection")) {
                    if (containsIgnoreCase(b, vs, ve, "close")) connClose = true;
                    if (containsIgnoreCase(b, vs, ve, "keep-alive")) connKeepAlive = true;
                } else if (nl == 17 && nameIs(b, line, "transfer-encoding")) {
                    if (teFrom >= 0) return reject(c, Out.S400, E_BAD_REQUEST, end - p);
                    teFrom = vs;
                    teTo = ve;
                }
            }
            line = le + 2;
        }
        int msgEnd;
        boolean chunked = teFrom >= 0;
        if (chunked) {
            // Transfer-Encoding with Content-Length, or on HTTP/1.0, is how requests get smuggled
            // past a proxy (RFC 9112 6.1), so both are refused rather than guessed at.
            if (!http11 || hasLength) return reject(c, Out.S400, E_BAD_REQUEST, end - p);
            if (teTo - teFrom != 7 || !nameIs(b, teFrom, "chunked")) {
                boolean last = teTo - teFrom > 7 && nameIs(b, teTo - 7, "chunked");
                return reject(c, last ? Out.S501 : Out.S400, last ? E_NOT_IMPLEMENTED : E_BAD_REQUEST, end - p);
            }
            msgEnd = chunked(b, he, end, false);
            if (msgEnd == MALFORMED) return reject(c, Out.S400, E_BAD_REQUEST, end - p);
            if (msgEnd == TOO_LARGE) return reject(c, Out.S413, E_TOO_LARGE, end - p);
            if (msgEnd == INCOMPLETE) return 0;
            bodyLen = chunkedLen;
        } else {
            if (contentLength > MAX_BODY) return reject(c, Out.S413, E_TOO_LARGE, end - p);
            msgEnd = he + (int) contentLength;
            if (end < msgEnd) return 0;
            bodyLen = (int) contentLength;
        }
        int total = msgEnd - p;
        bodyFrom = he;

        int route = route(b, isGet, isPost);
        if ((route == CREATE || route == LIKE) && db.writesHeld()) return HOLD;
        // Only now, since a held request is parsed again from its raw bytes.
        if (chunked) chunked(b, he, msgEnd, true);
        c.closeAfterWrite = http11 ? connClose : !connKeepAlive;
        out.close = c.closeAfterWrite;
        int mark = out.pos;
        try {
            handle(route, b);
        } catch (Throwable t) {
            System.err.println("request failed: " + t);
            out.pos = mark;
            out.respond(Out.S500, E_INTERNAL);
        }
        return total;
    }

    private static final int INCOMPLETE = 0, MALFORMED = -1, TOO_LARGE = -2;
    private int chunkedLen;

    // Walks a chunked body starting at b[from] and returns the index just past it, or INCOMPLETE,
    // MALFORMED or TOO_LARGE. Sets chunkedLen to the decoded length; with compact it also moves the
    // chunk data down so the decoded body is b[from, from + chunkedLen). Extensions and trailer
    // fields are skipped.
    private int chunked(byte[] b, int from, int end, boolean compact) {
        int k = from, len = 0;
        while (true) {
            int size = 0, start = k, d;
            while (k < end && (d = Character.digit(b[k], 16)) >= 0) {
                size = size * 16 + d;
                if (len + size > MAX_BODY) return TOO_LARGE;
                k++;
            }
            if (k == end) return INCOMPLETE;
            if (k == start || b[k] != '\r' && b[k] != ';' && b[k] != ' ' && b[k] != '\t') return MALFORMED;
            int eol = indexOf(b, (byte) '\r', k, end);
            if (eol < 0 || eol + 1 == end) return INCOMPLETE;
            if (b[eol + 1] != '\n') return MALFORMED;
            k = eol + 2;
            if (size == 0) break;
            if (end - k < size + 2) return INCOMPLETE;
            if (b[k + size] != '\r' || b[k + size + 1] != '\n') return MALFORMED;
            if (compact) System.arraycopy(b, k, b, from + len, size);
            len += size;
            k += size + 2;
        }
        while (true) {
            int eol = indexOf(b, (byte) '\r', k, end);
            if (eol < 0 || eol + 1 == end) return INCOMPLETE;
            if (b[eol + 1] != '\n') return MALFORMED;
            if (eol == k) {
                chunkedLen = len;
                return eol + 2;
            }
            k = eol + 2;
        }
    }

    private int reject(Conn c, byte[] status, byte[] body, int consumed) {
        c.closeAfterWrite = true;
        out.close = true;
        out.respond(status, body);
        return Math.max(consumed, 1);
    }

    private int route(byte[] b, boolean isGet, boolean isPost) {
        int len = pathTo - pathFrom;
        if (isGet) {
            if (len == 7 && startsWith(b, pathFrom, "/health")) return HEALTH;
            if (len == 5 && startsWith(b, pathFrom, "/feed")) return FEED;
            if (len > 7 && startsWith(b, pathFrom, "/posts/") && indexOf(b, (byte) '/', pathFrom + 7, pathTo) < 0) {
                return GET_POST;
            }
            return NOT_FOUND;
        }
        if (isPost) {
            if (len == 6 && startsWith(b, pathFrom, "/posts")) return CREATE;
            if (len > 12 && startsWith(b, pathFrom, "/posts/") && endsWith(b, pathTo, "/like")
                    && indexOf(b, (byte) '/', pathFrom + 7, pathTo - 5) < 0) {
                return LIKE;
            }
        }
        return NOT_FOUND;
    }

    private void handle(int route, byte[] b) {
        switch (route) {
            case HEALTH -> health();
            case FEED -> db.feed(out);
            case GET_POST -> {
                long id = parseId(b, pathFrom + 7, pathTo);
                if (id == -1) out.respond(Out.S400, E_INVALID_ID);
                else if (id == -2 || !db.post(id, out)) out.respond(Out.S404, E_NO_POST);
            }
            case CREATE -> create(b);
            case LIKE -> like(b);
            default -> out.respond(Out.S404, E_NOT_FOUND);
        }
    }

    private void health() {
        String error = db.ping();
        if (error == null) {
            long up = (System.nanoTime() - startNanos) / 1_000_000_000L;
            out.head(Out.S200, HEALTH_OK.length + Out.digits(up) + 1);
            out.put(HEALTH_OK);
            out.putLong(up);
            out.put(CLOSE_BRACE);
        } else {
            byte[] body = ("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":" + quote(error) + "}")
                    .getBytes(StandardCharsets.UTF_8);
            out.respond(Out.S503, body);
        }
    }

    private boolean auth(byte[] b) {
        int r = jwt.verify(b, authFrom, authTo);
        if (r == Jwt.OK) return true;
        out.respond(Out.S401, r == Jwt.MISSING ? E_MISSING_TOKEN : r == Jwt.INVALID ? E_INVALID_TOKEN : E_BAD_PAYLOAD);
        return false;
    }

    private void create(byte[] b) {
        if (!auth(b)) return;
        Object doc;
        try {
            doc = Json.parse(Json.utf8(b, bodyFrom, bodyLen));
        } catch (Json.Malformed e) {
            out.respond(Out.S400, E_MALFORMED);
            return;
        }
        if (!(Json.get(doc, "body") instanceof String raw)) {
            out.respond(Out.S400, E_BODY_REQUIRED);
            return;
        }
        String body = trim(raw);
        if (body.isEmpty()) {
            out.respond(Out.S400, E_BODY_REQUIRED);
            return;
        }
        if (body.codePointCount(0, body.length()) > 500) {
            out.respond(Out.S400, E_BODY_TOO_LONG);
            return;
        }
        db.insertPost(jwt.userId, body.getBytes(StandardCharsets.UTF_8), jwt.username, out);
    }

    private void like(byte[] b) {
        if (!auth(b)) return;
        long id = parseId(b, pathFrom + 7, pathTo - 5);
        if (id == -1) {
            out.respond(Out.S400, E_INVALID_ID);
            return;
        }
        int status = id == -2 ? 404 : db.like(jwt.userId, id);
        if (status == 404) {
            out.respond(Out.S404, E_NO_POST);
            return;
        }
        byte[] prefix = status == 201 ? LIKED_NEW : LIKED_AGAIN;
        out.head(status == 201 ? Out.S201 : Out.S200, prefix.length + Out.digits(id) + 1);
        out.put(prefix);
        out.putLong(id);
        out.put(CLOSE_BRACE);
    }

    private static final byte[] CLOSE_BRACE = {'}'};

    private static long parseId(byte[] b, int from, int to) {
        if (from >= to) return -1;
        long v = 0;
        boolean overflow = false;
        for (int k = from; k < to; k++) {
            int d = b[k] - '0';
            if (d < 0 || d > 9) return -1;
            if (!overflow) {
                if (v > (Long.MAX_VALUE - d) / 10) overflow = true;
                else v = v * 10 + d;
            }
        }
        if (overflow) return -2;
        return v > 0 ? v : -1;
    }

    private static boolean isJsSpace(char c) {
        return c == ' ' || (c >= '\t' && c <= '\r') || c == ' ' || c == ' '
                || (c >= ' ' && c <= ' ') || c == ' ' || c == ' ' || c == ' '
                || c == ' ' || c == '　' || c == '﻿';
    }

    private static String trim(String s) {
        int a = 0, z = s.length();
        while (a < z && isJsSpace(s.charAt(a))) a++;
        while (z > a && isJsSpace(s.charAt(z - 1))) z--;
        return s.substring(a, z);
    }

    private static String quote(String s) {
        StringBuilder sb = new StringBuilder(s.length() + 2).append('"');
        for (int k = 0; k < s.length(); k++) {
            char c = s.charAt(k);
            if (c == '"' || c == '\\') sb.append('\\').append(c);
            else if (c < 0x20) sb.append(String.format("\\u%04x", (int) c));
            else sb.append(c);
        }
        return sb.append('"').toString();
    }

    private static int headerEnd(byte[] b, int p, int end) {
        for (int k = p + 3; k < end; k++) {
            if (b[k] == '\n' && b[k - 1] == '\r' && b[k - 2] == '\n' && b[k - 3] == '\r') return k + 1;
        }
        return -1;
    }

    private static int indexOf(byte[] b, byte x, int from, int to) {
        for (int k = from; k < to; k++) if (b[k] == x) return k;
        return -1;
    }

    private static boolean startsWith(byte[] b, int at, String s) {
        for (int k = 0; k < s.length(); k++) if (b[at + k] != s.charAt(k)) return false;
        return true;
    }

    private static boolean endsWith(byte[] b, int to, String s) {
        return startsWith(b, to - s.length(), s);
    }

    private static boolean nameIs(byte[] b, int at, String lower) {
        for (int k = 0; k < lower.length(); k++) if ((b[at + k] | 0x20) != lower.charAt(k)) return false;
        return true;
    }

    private static boolean containsIgnoreCase(byte[] b, int from, int to, String lower) {
        int n = lower.length();
        for (int k = from; k + n <= to; k++) if (nameIs(b, k, lower)) return true;
        return false;
    }
}
