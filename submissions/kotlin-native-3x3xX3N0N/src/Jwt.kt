package app

/**
 * HMAC-SHA256 with the key's inner and outer pad states computed once at start (the per-token work is the message
 * blocks plus two final blocks). Idea credit: the precomputed ipad/opad midstate is a common trick on the board
 * (zig/asm entries); the SHA-256 code is FIPS 180-4 written out here.
 */
class HmacSha256(key: ByteArray) {
    private val inner = IntArray(8)
    private val outer = IntArray(8)
    private val st = IntArray(8)
    private val w = IntArray(64)
    private val blk = ByteArray(64)
    private val ih = ByteArray(32)

    companion object {
        private val K = intArrayOf(
            0x428a2f98, 0x71374491, -0x4a3f0431, -0x164a245b, 0x3956c25b, 0x59f111f1, -0x6dc07d5c, -0x54e3a12b,
            -0x27f85568, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, -0x7f214e02, -0x6423f959, -0x3e640e8c,
            -0x1b64963f, -0x1041b87a, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            -0x67c1aeae, -0x57ce3993, -0x4ffcd838, -0x40a68039, -0x391ff40d, -0x2a586eb9, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, -0x7e3d36d2, -0x6d8dd37b,
            -0x5d40175f, -0x57e599b5, -0x3db47490, -0x3893ae5d, -0x2e6d17e7, -0x2966f9dc, -0xbf1ca7b, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, -0x7b3787ec, -0x7338fdf8, -0x6f410006, -0x5baf9315, -0x41065c09, -0x398e870e,
        )
        private val H0 = intArrayOf(
            0x6a09e667, -0x4498517b, 0x3c6ef372, -0x5ab00ac6, 0x510e527f, -0x64fa9774, 0x1f83d9ab, 0x5be0cd19,
        )

        fun sha256(m: ByteArray): ByteArray {
            val h = HmacSha256(ByteArray(0))
            val s = H0.copyOf()
            val out = ByteArray(32)
            h.finish(s, m, 0, m.size, 0, out)
            return out
        }
    }

    init {
        val k = if (key.size > 64) sha256(key) else key
        val pad = ByteArray(64)
        for (i in 0 until 64) pad[i] = ((if (i < k.size) k[i].toInt() else 0) xor 0x36).toByte()
        H0.copyInto(inner); compress(inner, pad, 0)
        for (i in 0 until 64) pad[i] = ((if (i < k.size) k[i].toInt() else 0) xor 0x5c).toByte()
        H0.copyInto(outer); compress(outer, pad, 0)
    }

    /** out[0, 32) = HMAC-SHA256(key, b[off, off + len)). */
    fun mac(b: ByteArray, off: Int, len: Int, out: ByteArray) {
        inner.copyInto(st)
        finish(st, b, off, len, 64, ih)
        outer.copyInto(st)
        finish(st, ih, 0, 32, 64, out)
    }

    /** Hashes b[off, off + len) into state s (which already absorbed `prefix` bytes) and writes the digest. */
    private fun finish(s: IntArray, b: ByteArray, off: Int, len: Int, prefix: Int, out: ByteArray) {
        var p = off
        val end = off + len
        while (end - p >= 64) { compress(s, b, p); p += 64 }
        val rest = end - p
        b.copyInto(blk, 0, p, end)
        blk[rest] = 0x80.toByte()
        if (rest >= 56) {
            blk.fill(0, rest + 1, 64)
            compress(s, blk, 0)
            blk.fill(0, 0, 56)
        } else {
            blk.fill(0, rest + 1, 56)
        }
        val bits = (prefix + len).toLong() * 8
        for (i in 0 until 8) blk[63 - i] = (bits ushr (8 * i)).toByte()
        compress(s, blk, 0)
        for (i in 0 until 8) {
            val v = s[i]
            out[4 * i] = (v ushr 24).toByte(); out[4 * i + 1] = (v ushr 16).toByte()
            out[4 * i + 2] = (v ushr 8).toByte(); out[4 * i + 3] = v.toByte()
        }
    }

    private fun compress(s: IntArray, b: ByteArray, off: Int) {
        for (t in 0 until 16) {
            val i = off + 4 * t
            w[t] = (b[i].toInt() and 0xff shl 24) or (b[i + 1].toInt() and 0xff shl 16) or
                (b[i + 2].toInt() and 0xff shl 8) or (b[i + 3].toInt() and 0xff)
        }
        for (t in 16 until 64) {
            val x = w[t - 15]; val y = w[t - 2]
            val s0 = x.rotateRight(7) xor x.rotateRight(18) xor (x ushr 3)
            val s1 = y.rotateRight(17) xor y.rotateRight(19) xor (y ushr 10)
            w[t] = w[t - 16] + s0 + w[t - 7] + s1
        }
        var a = s[0]; var bb = s[1]; var c = s[2]; var d = s[3]
        var e = s[4]; var f = s[5]; var g = s[6]; var h = s[7]
        for (t in 0 until 64) {
            val t1 = h + (e.rotateRight(6) xor e.rotateRight(11) xor e.rotateRight(25)) + ((e and f) xor (e.inv() and g)) + K[t] + w[t]
            val t2 = (a.rotateRight(2) xor a.rotateRight(13) xor a.rotateRight(22)) + ((a and bb) xor (a and c) xor (bb and c))
            h = g; g = f; f = e; e = d + t1
            d = c; c = bb; bb = a; a = t1 + t2
        }
        s[0] += a; s[1] += bb; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h
    }
}

/** HS256 JWT verification straight off the request buffer (same rules as the JVM entry's Jwt.kt). Every request is
 *  verified; nothing is remembered. */
class Jwt(secret: String) {
    private val mac = HmacSha256(secret.encodeToByteArray())
    private val expected = ByteArray(32)
    private val given = ByteArray(64)

    var userId = 0L; private set
    var username: String = ""; private set

    companion object {
        const val OK = 0
        const val MISSING = 1     // missing bearer token
        const val INVALID = 2     // invalid or expired token
        const val PAYLOAD = 3     // invalid token payload

        private val B64 = IntArray(256) { -1 }.also { t ->
            val a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
            for (k in a.indices) t[a[k].code] = k
        }

        /** base64url (no padding) decode of b[s, e) into out; returns length or -1. */
        fun b64(b: ByteArray, s: Int, e: Int, out: ByteArray): Int {
            val n = e - s
            if (n % 4 == 1) return -1
            val outLen = n * 3 / 4
            if (outLen > out.size) return -1
            var o = 0; var acc = 0; var bits = 0
            for (k in s until e) {
                val v = B64[b[k].toInt() and 0xFF]
                if (v < 0) return -1
                acc = (acc shl 6) or v; bits += 6
                if (bits >= 8) { bits -= 8; out[o++] = (acc shr bits).toByte() }
            }
            return o
        }

        private val BEARER = "Bearer ".encodeToByteArray()
    }

    /** Verifies the Authorization header value at b[s, e) (s < 0 means header absent). */
    fun verify(b: ByteArray, s: Int, e: Int, nowSec: Long): Int {
        if (s < 0 || e - s < BEARER.size) return MISSING
        for (k in BEARER.indices) if (b[s + k] != BEARER[k]) return MISSING
        var ts = s + BEARER.size
        var te = e
        while (ts < te && b[ts] == ' '.code.toByte()) ts++
        while (te > ts && b[te - 1] == ' '.code.toByte()) te--
        var d1 = -1; var d2 = -1
        for (k in ts until te) if (b[k] == '.'.code.toByte()) {
            if (d1 < 0) d1 = k else if (d2 < 0) d2 = k else return INVALID
        }
        if (d2 < 0) return INVALID

        // Signature over "header.payload".
        mac.mac(b, ts, d2 - ts, expected)
        if (b64(b, d2 + 1, te, given) != 32) return INVALID
        var diff = 0 // constant-time compare
        for (k in 0 until 32) diff = diff or (expected[k].toInt() xor given[k].toInt())
        if (diff != 0) return INVALID

        val header: Any; val payload: Any
        try {
            val hb = ByteArray(d1 - ts); val hl = b64(b, ts, d1, hb); if (hl < 0) return INVALID
            header = Json.parse(hb, 0, hl)
            val pb = ByteArray(d2 - d1); val pl = b64(b, d1 + 1, d2, pb); if (pl < 0) return INVALID
            payload = Json.parse(pb, 0, pl)
        } catch (_: Json.Bad) {
            return INVALID
        }
        if (header !is Map<*, *> || header["alg"] != "HS256") return INVALID
        if (payload !is Map<*, *>) return INVALID
        // SPEC: verify `exp`. A token without a numeric `exp` is rejected, not treated as non-expiring.
        val exp = payload["exp"]
        if (exp !is Double || exp <= nowSec.toDouble()) return INVALID
        val nbf = payload["nbf"]
        if (nbf is Double && nbf > nowSec.toDouble()) return INVALID

        val sub = payload["sub"]
        val name = payload["username"]
        if (sub !is String || name !is String) return PAYLOAD
        if (sub.isEmpty() || sub.length > 18 || sub[0] == '0') return PAYLOAD
        var id = 0L
        for (c in sub) { if (c !in '0'..'9') return PAYLOAD; id = id * 10 + (c - '0') }
        userId = id
        username = name
        return OK
    }
}
