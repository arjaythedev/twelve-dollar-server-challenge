package app;

import java.lang.foreign.MemorySegment;
import javax.crypto.Mac;

/**
 * A read context: one SQLite connection with its persistent statements, plus the per-thread HMAC
 * state. Borrowed for the SQL part of a request and given back immediately -- the code between
 * acquire and release never parks (no locks, no logging, no socket I/O), so a handful of these
 * saturate a single carrier thread.
 */
final class Ctx {
    final MemorySegment db;
    final MemorySegment stFeed, stPost, stExists, stValidate, stHealth;
    final Mac mac;
    final JsonScan json = new JsonScan();
    final byte[] macIn = new byte[8192];   // "header.payload" bytes, copied out of the request buffer
    final byte[] macOut = new byte[32];
    final byte[] sigEnc = new byte[43];    // base64url of macOut, compared with the token's signature

    Ctx(MemorySegment db, MemorySegment stFeed, MemorySegment stPost, MemorySegment stExists,
        MemorySegment stValidate, MemorySegment stHealth, Mac mac) {
        this.db = db;
        this.stFeed = stFeed;
        this.stPost = stPost;
        this.stExists = stExists;
        this.stValidate = stValidate;
        this.stHealth = stHealth;
        this.mac = mac;
    }
}
