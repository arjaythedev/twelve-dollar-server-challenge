package server

/** A reusable, growable byte buffer that writes compact JSON as UTF-8. */
class JsonOut {
    private var buf = ByteArray(8192)
    private var len = 0

    fun reset() { len = 0 }

    fun toByteArray(): ByteArray = buf.copyOf(len)

    private fun ensure(extra: Int) {
        if (len + extra > buf.size) buf = buf.copyOf(maxOf(buf.size * 2, len + extra))
    }

    fun byte(b: Int) {
        ensure(1)
        buf[len++] = b.toByte()
    }

    /** Appends an ASCII literal as is. */
    fun raw(s: String) {
        ensure(s.length)
        for (i in s.indices) buf[len++] = s[i].code.toByte()
    }

    fun num(n: Long) {
        if (n in 0..9) return byte('0'.code + n.toInt())
        raw(n.toString())
    }

    /** A JSON string from the first [n] UTF-8 bytes of [s]: only `"`, `\` and control characters need escaping. */
    fun strBytes(s: ByteArray, n: Int = s.size) {
        ensure(n * 6 + 2) // worst case: every byte escaped as \u00XX
        buf[len++] = '"'.code.toByte()
        for (i in 0 until n) {
            val b = s[i]
            val c = b.toInt()
            if (c == '"'.code || c == '\\'.code || (c in 0..0x1f)) escape(c) else buf[len++] = b
        }
        buf[len++] = '"'.code.toByte()
    }

    fun str(s: String) = strBytes(s.toByteArray(Charsets.UTF_8))

    private fun escape(c: Int) {
        when (c) {
            '"'.code -> raw("\\\"")
            '\\'.code -> raw("\\\\")
            '\n'.code -> raw("\\n")
            '\r'.code -> raw("\\r")
            '\t'.code -> raw("\\t")
            '\b'.code -> raw("\\b")
            0x0c -> raw("\\f")
            else -> raw("\\u00" + HEX[c shr 4] + HEX[c and 0xf])
        }
    }

    private companion object {
        const val HEX = "0123456789abcdef"
    }
}
