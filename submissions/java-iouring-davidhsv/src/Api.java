/** Routing, responses, the read plans over SQLite's B-tree cursors, the writes, and SQLite's
 * record format. */
final class Api {
    private Api() {}

    // The trees db.c keeps a cursor on during a pass.
    static final int POSTS = 0, USERS = 1, FEED = 2 /* posts_created_at_id_idx */, LIKES = 3 /* likes_post_id_idx */;

    static final int FEED_LEN = 20;
    // Like counts come from one walk of the likes index when the feed's ids span at most this many.
    static final int LIKE_SPAN = 512;
    static final int MAX_CHARS = 500;

    // Scratch, reused from request to request as storage only: each is cleared before use, and
    // nothing read from the database outlives the request that read it.
    static final Buf body = new Buf(65536);   // a response's body
    static final Buf text = new Buf(65536);   // record bytes copied out of SQLite for the feed being served
    static final Buf unesc = new Buf(4096);   // a decoded JSON string

    static long startedUs;

    // ---- responses

    private static byte[] response(String status, String payload) {
        byte[] p = Out.ascii(payload);
        return Out.ascii("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: " + p.length
                + "\r\n\r\n" + payload);
    }

    static final byte[] R_BAD_REQUEST = response("400 Bad Request", "{\"error\":\"bad request\"}");
    static final byte[] R_INVALID_ID = response("400 Bad Request", "{\"error\":\"invalid post id\"}");
    static final byte[] R_MALFORMED = response("400 Bad Request", "{\"error\":\"malformed JSON body\"}");
    static final byte[] R_BODY_REQUIRED = response("400 Bad Request", "{\"error\":\"body is required\"}");
    static final byte[] R_TOO_LONG = response("400 Bad Request", "{\"error\":\"body must be at most 500 characters\"}");
    static final byte[] R_NO_TOKEN = response("401 Unauthorized", "{\"error\":\"missing bearer token\"}");
    static final byte[] R_BAD_TOKEN = response("401 Unauthorized", "{\"error\":\"invalid or expired token\"}");
    static final byte[] R_BAD_PAYLOAD = response("401 Unauthorized", "{\"error\":\"invalid token payload\"}");
    static final byte[] R_NOT_FOUND = response("404 Not Found", "{\"error\":\"not found\"}");
    static final byte[] R_POST_NOT_FOUND = response("404 Not Found", "{\"error\":\"post not found\"}");
    static final byte[] R_500 = response("500 Internal Server Error", "{\"error\":\"internal server error\"}");

    private static final byte[] H_200 = Out.ascii("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ");
    private static final byte[] H_201 = Out.ascii("HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: ");
    private static final byte[] H_503 = Out.ascii("HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: ");
    private static final byte[] CRLF2 = Out.ascii("\r\n\r\n");

    /** Appends one whole response, head and the body in `body`, to the connection's output; it
     * leaves after the pass's COMMIT. */
    static void answer(int ci, byte[] head) {
        Server.touch(ci);
        Server.flags[ci] |= Server.F_ANSWERED;
        long n = body.len;
        int w = Out.width(n);
        long at = N.outReserve(Server.ringOf(ci), Server.slotOf(ci), head.length + w + 4 + n);
        N.copy(head, 0, at, head.length);
        at += head.length;
        Out.digits(at, n);
        at += w;
        N.copy(CRLF2, 0, at, 4);
        N.copy(body.base, at + 4, n);
    }

    /** Appends a response whose bytes are all known in advance. */
    static void answer(int ci, byte[] whole, boolean unused) {
        Server.touch(ci);
        Server.flags[ci] |= Server.F_ANSWERED;
        N.copy(whole, 0, N.outReserve(Server.ringOf(ci), Server.slotOf(ci), whole.length), whole.length);
    }

    static void fixed(int ci, byte[] whole) { answer(ci, whole, true); }

    // ---- routing

    private static final byte[] P_FEED = Out.ascii("/feed");
    private static final byte[] P_HEALTH = Out.ascii("/health");
    private static final byte[] P_POSTS = Out.ascii("/posts");
    private static final byte[] P_POSTS_ = Out.ascii("/posts/");
    private static final byte[] P_LIKE = Out.ascii("/like");

    private static long position(long a, long end, int c) {
        for (long p = a; p < end; p++) if (N.u(p) == c) return p;
        return -1;
    }

    /** Answers the request Http.parse just read. True when it is a write that now waits in the
     * write queue. */
    static boolean handle(int ci) {
        long p = Http.p0, pn = Http.p1 - Http.p0;
        if (Http.get) {
            if (Out.eq(p, pn, P_FEED)) { feed(ci); return false; }
            if (Out.startsWith(p, pn, P_POSTS_)) {
                long rest = p + 7;
                if (position(rest, Http.p1, '/') < 0) {
                    long id = positiveInt(rest, Http.p1 - rest);
                    if (id <= 0) fixed(ci, R_INVALID_ID);
                    else getPost(ci, id);
                    return false;
                }
            }
            if (Out.eq(p, pn, P_HEALTH)) { health(ci); return false; }
        } else if (Http.post) {
            if (Out.eq(p, pn, P_POSTS)) return createPost(ci);
            if (pn >= 12 && Out.startsWith(p, pn, P_POSTS_) && Out.eq(Http.p1 - 5, 5, P_LIKE)) {
                long rest = p + 7, end = Http.p1 - 5;
                if (position(rest, end, '/') < 0) return likePost(ci, rest, end - rest);
            }
        }
        fixed(ci, R_NOT_FOUND);
        return false;
    }

    /** A positive decimal integer with no sign, spaces or leading zeros that fits a long; else 0. */
    static long positiveInt(long s, long n) {
        if (n == 0 || n > 18 || N.u(s) == '0') return 0;
        long v = 0;
        for (long p = s; p < s + n; p++) {
            int c = N.u(p);
            if (c < '0' || c > '9') return 0;
            v = v * 10 + (c - '0');
        }
        return v;
    }

    // ---- SQLite's record format: a header of varints (its own length, then one serial type per
    // column), then the column values in order.

    private static final int[] SERIAL_SIZES = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};

    static long serialSize(long t) { return t >= 12 ? (t - 12) / 2 : t >= 0 ? SERIAL_SIZES[(int) t] : 0; }

    // What columns() found: serial types, and where each value starts and ends (addresses).
    static final long[] ct = new long[4], cs = new long[4], ce = new long[4];
    private static long vintValue;

    /** The varint at i (before end): its value in vintValue and the position after it, or -1. */
    private static long vint(long i, long end) {
        if (i < end && N.u(i) < 0x80) { vintValue = N.u(i); return i + 1; }
        long v = 0;
        for (int k = 0; k < 9; k++) {
            if (i + k >= end) return -1;
            int c = N.u(i + k);
            if (k == 8) { vintValue = (v << 8) | c; return i + 9; }
            v = (v << 7) | (c & 0x7f);
            if (c < 0x80) { vintValue = v; return i + k + 1; }
        }
        return -1;
    }

    /** The first n (at most 4) columns of the len-byte record at r. */
    static boolean columns(long r, long len, int n) {
        // Short headers (every record of this schema): the header's length is one byte, and a
        // serial type is one or two.
        if (len < 1 || N.u(r) >= 0x80) return columnsAny(r, len, n);
        long hsize = N.u(r);
        if (hsize > len) return false;
        long h = 1;
        long v = hsize;
        for (int k = 0; k < n; k++) {
            if (h >= hsize) return false;
            long t = N.u(r + h);
            h++;
            if (t >= 0x80) {
                if (h >= hsize) return false;
                long low = N.u(r + h);
                h++;
                if (low >= 0x80) return columnsAny(r, len, n);
                t = ((t & 0x7f) << 7) | low;
            }
            long size = serialSize(t);
            if (v + size > len) return false;
            ct[k] = t;
            cs[k] = r + v;
            ce[k] = r + v + size;
            v += size;
        }
        return true;
    }

    /** The same for any record. */
    private static boolean columnsAny(long r, long len, int n) {
        long h = vint(r, r + len);
        long hsize = vintValue;
        if (h < 0 || hsize > len) return false;
        long v = hsize;
        for (int k = 0; k < n; k++) {
            if (h >= r + hsize) return false;
            long next = vint(h, r + hsize);
            if (next < 0) return false;
            long t = vintValue;
            h = next;
            long size = serialSize(t);
            if (v + size > len) return false;
            ct[k] = t;
            cs[k] = r + v;
            ce[k] = r + v + size;
            v += size;
        }
        return true;
    }

    static boolean isText(long t) { return t >= 13 && (t & 1) == 1; }

    private static boolean intOk;

    /** A column as an integer (serial types 1-6, 8 and 9); intOk false otherwise. */
    static long intColumn(long t, long s) {
        intOk = true;
        if (t == 8) return 0;
        if (t == 9) return 1;
        if (t < 1 || t > 6) { intOk = false; return 0; }
        long n = serialSize(t);
        long v = N.b(s);
        for (long k = 1; k < n; k++) v = (v << 8) | N.u(s + k);
        return v;
    }

    // ---- reads. Each moves SQLite's cursors inside the pass's transaction and builds its response
    // from the rows it reads there; nothing is remembered between requests.

    /** The leading integer column of the index entry the cursor is on (post_id in
     * likes_post_id_idx, whose entries are two integers: at most 19 bytes); intOk false on an
     * error. */
    static long indexKey(int tree) {
        long r = N.rec(tree);
        if (r == 0) { intOk = false; return 0; }
        long len = N.getLong(N.A.IO_LEN);
        if (len >= 3 && len <= 24) {
            int at = N.u(r), t = N.u(r + 1);
            if (at >= 2 && at <= 16 && t <= 9) {
                intOk = true;
                if (t == 8) return 0;
                if (t == 9) return 1;
                if (t < 1 || t > 6 || at + SERIAL_SIZES[t] > len) { intOk = false; return 0; }
                return intColumn(t, r + at);
            }
        }
        // An entry of another shape: decode the whole record.
        if (!columns(r, len, 1)) { intOk = false; return 0; }
        return intColumn(ct[0], cs[0]);
    }

    /** The likes of one post: its entries in likes_post_id_idx (post_id, rowid). -1 on an error. */
    static long countLikes(long id) {
        int at = N.seekGe(LIKES, id);
        long n = 0;
        while (at == 1) {
            long v = indexKey(LIKES);
            if (!intOk) return -1;
            if (v != id) break;
            n++;
            at = N.next(LIKES);
        }
        return at < 0 ? -1 : n;
    }

    // Where author() found the username: on the users cursor's page, good until it moves.
    private static long authorAt, authorLen;

    /** The username of user uid: 1 (authorAt, authorLen), 0 no such user, -1 an error. */
    static int author(long uid) {
        int at = N.seek(USERS, uid);
        if (at != 1) return at;
        long r = N.rec(USERS);
        if (r == 0 || !columns(r, N.getLong(N.A.IO_LEN), 2) || !isText(ct[1])) return -1;
        authorAt = cs[1];
        authorLen = ce[1] - cs[1];
        return 1;
    }

    private static final byte[] J_POST = Out.ascii("{\"post\":");

    static void getPost(int ci, long id) {
        if (N.begin() != 0) { fixed(ci, R_500); return; }
        int at = N.seek(POSTS, id);
        if (at == 0) { fixed(ci, R_POST_NOT_FOUND); return; }
        if (at < 0) { fixed(ci, R_500); return; }
        // The post's record stays on its page while the other cursors move.
        long r = N.rec(POSTS);
        // posts: id (the rowid, stored as NULL), user_id, body, created_at
        if (r == 0 || !columns(r, N.getLong(N.A.IO_LEN), 4) || !isText(ct[2]) || !isText(ct[3])) { fixed(ci, R_500); return; }
        long b0 = cs[2], b1 = ce[2], c1 = ce[3];
        long uid = intColumn(ct[1], cs[1]);
        if (!intOk) { fixed(ci, R_500); return; }
        int found = author(uid);
        if (found == 0) { fixed(ci, R_POST_NOT_FOUND); return; }  // the JOIN finds no row
        if (found < 0) { fixed(ci, R_500); return; }
        long likes = countLikes(id);
        if (likes < 0) { fixed(ci, R_500); return; }
        body.clear();
        body.put(J_POST);
        Out.post(body, id, b0, b1 - b0, b1, c1 - b1, authorAt, authorLen, likes);
        body.put('}');
        answer(ci, H_200);
    }

    // A feed row: in `text`, the body is b0..b1, created_at b1..c1 and the author a0..a1 (offsets).
    private static final long[] rId = new long[FEED_LEN], rUid = new long[FEED_LEN], rB0 = new long[FEED_LEN],
            rB1 = new long[FEED_LEN], rC1 = new long[FEED_LEN], rA0 = new long[FEED_LEN], rA1 = new long[FEED_LEN],
            rLikes = new long[FEED_LEN];
    private static final int[] order = new int[FEED_LEN];
    private static int nrows;
    private static final int[] likeCounts = new int[LIKE_SPAN + 1];

    /** The 20 newest posts by (created_at DESC, id DESC), which is the order of
     * posts_created_at_id_idx, so the plan is: the index's first 20 entries, their posts by rowid
     * in ascending order (neighbours on a leaf are one step apart), their authors, their like
     * counts. Nothing about the data is assumed. 1 rows filled, 0 the reference query must
     * answer, -1 error. */
    static int feedRows() {
        text.clear();
        nrows = 0;
        int at = N.first(FEED);
        while (at == 1 && nrows < FEED_LEN) {
            long r = N.rec(FEED);
            // the index: created_at, id, rowid
            if (r == 0 || !columns(r, N.getLong(N.A.IO_LEN), 2)) return -1;
            long id = intColumn(ct[1], cs[1]);
            if (!intOk) return -1;
            rId[nrows] = id;
            nrows++;
            at = N.next(FEED);
        }
        if (at < 0) return -1;
        int n = nrows;
        // Newest first is normally descending ids, so ascending is the reverse.
        for (int k = 0; k < n; k++) order[k] = n - 1 - k;
        for (int k = 1; k < n; k++) {
            int o = order[k];
            int j = k;
            while (j > 0 && rId[order[j - 1]] > rId[o]) { order[j] = order[j - 1]; j--; }
            order[j] = o;
        }
        for (int k = 0; k < n; k++) {
            int row = order[k];
            int found = N.seek(POSTS, rId[row]);
            if (found < 0) return -1;
            if (found == 0) return 0;  // the index and the table disagree
            long r = N.rec(POSTS);
            if (r == 0 || !columns(r, N.getLong(N.A.IO_LEN), 4) || !isText(ct[2]) || !isText(ct[3])) return -1;
            long uid = intColumn(ct[1], cs[1]);
            if (!intOk) return -1;
            rUid[row] = uid;
            // body and created_at lie side by side in the record: one copy takes both
            long start = text.len;
            text.put(cs[2], ce[3] - cs[2]);
            rB0[row] = start;
            rB1[row] = start + (ce[2] - cs[2]);
            rC1[row] = text.len;
        }
        for (int k = 0; k < n; k++) {
            int found = author(rUid[k]);
            if (found < 0) return -1;
            if (found == 0) return 0;  // a post without a user: the JOIN drops it
            rA0[k] = text.len;
            text.put(authorAt, authorLen);
            rA1[k] = text.len;
        }
        if (n == 0) return 1;
        long lo = rId[order[0]];
        long hi = rId[order[n - 1]];
        if (hi - lo > LIKE_SPAN) {
            for (int k = 0; k < n; k++) {
                long likes = countLikes(rId[k]);
                if (likes < 0) return -1;
                rLikes[k] = likes;
            }
            return 1;
        }
        // One walk of the index from the lowest id to the highest counts them all.
        for (int k = 0; k <= hi - lo; k++) likeCounts[k] = 0;
        at = N.seekGe(LIKES, lo);
        while (at == 1) {
            long v = indexKey(LIKES);
            if (!intOk) return -1;
            if (v > hi) break;
            likeCounts[(int) (v - lo)]++;
            at = N.next(LIKES);
        }
        if (at < 0) return -1;
        for (int k = 0; k < n; k++) rLikes[k] = likeCounts[(int) (rId[k] - lo)];
        return 1;
    }

    private static final byte[] J_POSTS = Out.ascii("{\"posts\":[");
    private static final byte[] J_END = Out.ascii("]}");

    static void feed(int ci) {
        if (N.begin() != 0) { fixed(ci, R_500); return; }
        int got = feedRows();
        if (got < 0) { fixed(ci, R_500); return; }
        body.clear();
        body.put(J_POSTS);
        if (got == 1) {
            long t = text.base;
            for (int k = 0; k < nrows; k++) {
                if (k > 0) body.put(',');
                Out.post(body, rId[k], t + rB0[k], rB1[k] - rB0[k], t + rB1[k], rC1[k] - rB1[k], t + rA0[k],
                        rA1[k] - rA0[k], rLikes[k]);
            }
        } else if (!feedByQuery()) {
            fixed(ci, R_500);
            return;
        }
        body.put(J_END);
        answer(ci, H_200);
    }

    /** The reference SQL query, for a feed the cursor plan does not answer. */
    static boolean feedByQuery() {
        boolean first = true;
        for (;;) {
            int st = N.feedStep();
            if (st == 0) return true;
            if (st < 0) return false;
            if (!first) body.put(',');
            first = false;
            long tx = N.A.IO_TEXT, tl = N.A.IO_TEXT_LEN;
            Out.post(body, N.getLong(N.A.IO_ID), N.getLong(tx), N.getLong(tl), N.getLong(tx + 8), N.getLong(tl + 8),
                    N.getLong(tx + 16), N.getLong(tl + 16), N.getLong(N.A.IO_LIKES));
        }
    }

    private static final byte[] H_OK = Out.ascii("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    private static final byte[] H_BAD = Out.ascii("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
    private static final byte[] NO_BEGIN = Out.ascii("cannot begin a transaction");
    private static final byte[] QUOTE_END = Out.ascii("\"}");

    static void health(int ci) {
        body.clear();
        boolean began = N.begin() == 0;
        if (began && N.ping() == 0) {
            body.put(H_OK);
            body.num((System.nanoTime() / 1000 - startedUs) / 1000000);
            body.put('}');
            answer(ci, H_200);
            return;
        }
        body.put(H_BAD);
        if (!began) {
            body.put(NO_BEGIN);
        } else {
            long err = N.dbError();
            Out.text(body, err, N.cstrlen(err));
        }
        body.put(QUOTE_END);
        answer(ci, H_503);
    }

    // ---- writes

    /** Checks the token and answers its failures: the user id (the username is in Jwt.who), or 0. */
    static long authorize(int ci) {
        int why = Http.hasAuth ? Jwt.verify(Http.a0, Http.a1 - Http.a0, true) : Jwt.verify(0, 0, false);
        if (why == 0) return Jwt.uid;
        fixed(ci, why == 1 ? R_NO_TOKEN : why == 2 ? R_BAD_TOKEN : R_BAD_PAYLOAD);
        return 0;
    }

    private static final byte[] K_BODY = Out.ascii("body");
    private static long trim0, trim1;

    static boolean createPost(int ci) {
        long uid = authorize(ci);
        if (uid == 0) return false;
        int shape = Json.members(Http.b0, Http.b1, 1, K_BODY, null, null);
        if (shape < 0) { fixed(ci, R_MALFORMED); return false; }
        if (shape == 0 || Json.kind[0] != Json.STRING) { fixed(ci, R_BODY_REQUIRED); return false; }
        long raw, rawEnd;
        if (Json.esc[0]) {
            unesc.clear();
            Json.unescape(Json.s[0], Json.e[0], unesc);
            raw = unesc.base;
            rawEnd = unesc.base + unesc.len;
        } else {
            raw = Json.s[0];
            rawEnd = Json.e[0];
        }
        jsTrim(raw, rawEnd);
        if (trim0 == trim1) { fixed(ci, R_BODY_REQUIRED); return false; }
        long n = trim1 - trim0;
        if (n > MAX_CHARS && codePoints(trim0, trim1) > MAX_CHARS) { fixed(ci, R_TOO_LONG); return false; }
        if (Server.writesNow) { doPost(ci, uid, Jwt.who.base, Jwt.who.len, trim0, n); return false; }
        Server.queuePost(ci, uid, trim0, n);
        return true;
    }

    static boolean likePost(int ci, long idText, long idLen) {
        long uid = authorize(ci);
        if (uid == 0) return false;
        long id = positiveInt(idText, idLen);
        if (id <= 0) { fixed(ci, R_INVALID_ID); return false; }
        if (Server.writesNow) { doLike(ci, uid, id); return false; }
        Server.queueLike(ci, uid, id);
        return true;
    }

    static void doPost(int ci, long uid, long author, long authorLen, long txt, long txtLen) {
        if (N.begin() != 0) { fixed(ci, R_500); return; }
        if (N.addPost(uid, txt, txtLen) != 0) { fixed(ci, R_500); return; }
        body.clear();
        body.put(J_POST);
        Out.post(body, N.getLong(N.A.IO_ID), txt, txtLen, N.getLong(N.A.IO_TEXT), N.getLong(N.A.IO_TEXT_LEN), author,
                authorLen, 0);
        body.put('}');
        answer(ci, H_201);
    }

    private static final byte[] J_LIKED_NEW = Out.ascii("{\"liked\":true,\"already_liked\":false,\"post_id\":");
    private static final byte[] J_LIKED_AGAIN = Out.ascii("{\"liked\":true,\"already_liked\":true,\"post_id\":");

    static void doLike(int ci, long uid, long id) {
        if (N.begin() != 0) { fixed(ci, R_500); return; }
        int found = N.seek(POSTS, id);
        if (found == 0) { fixed(ci, R_POST_NOT_FOUND); return; }
        if (found < 0) { fixed(ci, R_500); return; }
        int added = N.addLike(uid, id);
        if (added < 0) { fixed(ci, R_500); return; }
        body.clear();
        body.put(added == 1 ? J_LIKED_NEW : J_LIKED_AGAIN);
        body.num(id);
        body.put('}');
        answer(ci, added == 1 ? H_201 : H_200);
    }

    /** JavaScript's String.prototype.trim: WhiteSpace and LineTerminator code points. */
    static boolean jsSpace(int cp) {
        if (cp <= 0x20) return cp == 0x20 || (cp >= 9 && cp <= 13);
        if (cp < 0xa0) return false;
        return cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029
                || cp == 0x202f || cp == 0x205f || cp == 0x3000 || cp == 0xfeff;
    }

    private static int cpLen;

    /** The code point of the (valid) UTF-8 sequence at i; its length in cpLen. */
    static int utf8At(long i) {
        int c = N.u(i);
        if (c < 0x80) { cpLen = 1; return c; }
        if (c < 0xe0) { cpLen = 2; return ((c & 0x1f) << 6) | (N.u(i + 1) & 0x3f); }
        if (c < 0xf0) { cpLen = 3; return ((c & 0x0f) << 12) | ((N.u(i + 1) & 0x3f) << 6) | (N.u(i + 2) & 0x3f); }
        cpLen = 4;
        return ((c & 0x07) << 18) | ((N.u(i + 1) & 0x3f) << 12) | ((N.u(i + 2) & 0x3f) << 6) | (N.u(i + 3) & 0x3f);
    }

    /** Where s..e (valid UTF-8) starts and ends once trimmed: trim0, trim1. */
    static void jsTrim(long s, long e) {
        long i = s, j = e;
        while (i < j) {
            int cp = utf8At(i);
            if (!jsSpace(cp)) break;
            i += cpLen;
        }
        while (j > i) {
            long k = j - 1;
            while (k > i && (N.u(k) & 0xc0) == 0x80) k--;
            int cp = utf8At(k);
            if (!jsSpace(cp)) break;
            j = k;
        }
        trim0 = i;
        trim1 = j;
    }

    static long codePoints(long s, long e) {
        long n = 0;
        for (long p = s; p < e; p++) if ((N.u(p) & 0xc0) != 0x80) n++;
        return n;
    }
}
