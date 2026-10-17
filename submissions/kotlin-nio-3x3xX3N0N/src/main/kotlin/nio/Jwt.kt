package nio

import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/** HS256 JWT verification straight off the request buffer. Every request is verified; nothing is remembered. */
class Jwt(secret: String) {
    private val mac: Mac = Mac.getInstance("HmacSHA256").apply {
        init(SecretKeySpec(secret.toByteArray(Charsets.UTF_8), "HmacSHA256"))
    }
    private val expected = ByteArray(32)
    private val given = ByteArray(64)

    // Result of the last verify() call.
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

        private val BEARER = "Bearer ".toByteArray()
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
        mac.update(b, ts, d2 - ts)
        mac.doFinal(expected, 0)
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
        val exp = payload["exp"]
        if (exp != null && (exp !is Double || exp <= nowSec.toDouble())) return INVALID
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
