package nio

/**
 * Minimal strict JSON parser over a byte range. Only used for the tiny inputs this API reads
 * (POST /posts bodies and JWT header/payload); responses are built by SQLite.
 * Objects -> HashMap (last duplicate key wins), arrays -> ArrayList, numbers -> Double,
 * null -> Json.NULL. Throws Json.Bad on any syntax error.
 */
class Json private constructor(private val b: ByteArray, private var i: Int, private val end: Int) {
    object NULL
    class Bad : RuntimeException(null, null, false, false)

    companion object {
        private val BAD = Bad()

        fun parse(b: ByteArray, off: Int = 0, len: Int = b.size): Any {
            val p = Json(b, off, off + len)
            p.ws()
            val v = p.value(0)
            p.ws()
            if (p.i != p.end) throw BAD
            return v
        }
    }

    private fun ws() {
        while (i < end) {
            val c = b[i].toInt()
            if (c == 0x20 || c == 0x0A || c == 0x0D || c == 0x09) i++ else return
        }
    }

    private fun value(depth: Int): Any {
        if (i >= end || depth > 64) throw BAD
        return when (b[i].toInt().toChar()) {
            '{' -> obj(depth)
            '[' -> arr(depth)
            '"' -> str()
            't' -> lit("true", true)
            'f' -> lit("false", false)
            'n' -> lit("null", NULL)
            else -> num()
        }
    }

    private fun lit(s: String, v: Any): Any {
        if (end - i < s.length) throw BAD
        for (k in s.indices) if (b[i + k].toInt() != s[k].code) throw BAD
        i += s.length
        return v
    }

    private fun obj(depth: Int): Any {
        i++ // {
        val m = HashMap<String, Any>(8)
        ws()
        if (i < end && b[i] == '}'.code.toByte()) { i++; return m }
        while (true) {
            ws()
            if (i >= end || b[i] != '"'.code.toByte()) throw BAD
            val k = str()
            ws()
            if (i >= end || b[i] != ':'.code.toByte()) throw BAD
            i++
            ws()
            m[k] = value(depth + 1)
            ws()
            if (i >= end) throw BAD
            when (b[i].toInt().toChar()) {
                ',' -> i++
                '}' -> { i++; return m }
                else -> throw BAD
            }
        }
    }

    private fun arr(depth: Int): Any {
        i++ // [
        val a = ArrayList<Any>()
        ws()
        if (i < end && b[i] == ']'.code.toByte()) { i++; return a }
        while (true) {
            ws()
            a.add(value(depth + 1))
            ws()
            if (i >= end) throw BAD
            when (b[i].toInt().toChar()) {
                ',' -> i++
                ']' -> { i++; return a }
                else -> throw BAD
            }
        }
    }

    private fun num(): Any {
        val s = i
        if (i < end && b[i] == '-'.code.toByte()) i++
        if (i >= end) throw BAD
        if (b[i] == '0'.code.toByte()) i++
        else if (b[i] in '1'.code.toByte()..'9'.code.toByte()) { while (i < end && isDigit(b[i])) i++ }
        else throw BAD
        if (i < end && b[i] == '.'.code.toByte()) {
            i++
            if (i >= end || !isDigit(b[i])) throw BAD
            while (i < end && isDigit(b[i])) i++
        }
        if (i < end && (b[i] == 'e'.code.toByte() || b[i] == 'E'.code.toByte())) {
            i++
            if (i < end && (b[i] == '+'.code.toByte() || b[i] == '-'.code.toByte())) i++
            if (i >= end || !isDigit(b[i])) throw BAD
            while (i < end && isDigit(b[i])) i++
        }
        return String(b, s, i - s, Charsets.ISO_8859_1).toDouble()
    }

    private fun isDigit(x: Byte) = x >= '0'.code.toByte() && x <= '9'.code.toByte()

    private fun str(): String {
        i++ // opening quote
        val s = i
        // Fast path: no escapes, plain ASCII.
        while (i < end) {
            val c = b[i].toInt()
            if (c == '"'.code) { val r = String(b, s, i - s, Charsets.ISO_8859_1); i++; return r }
            if (c == '\\'.code || c < 0x20) break  // escape or control (control is rejected below)
            if (c < 0) break                       // non-ASCII -> slow path
            i++
        }
        i = s
        val sb = StringBuilder()
        var runStart = i
        while (true) {
            if (i >= end) throw BAD
            val c = b[i].toInt() and 0xFF
            when {
                c == '"'.code -> {
                    if (i > runStart) sb.append(String(b, runStart, i - runStart, Charsets.UTF_8))
                    i++
                    return sb.toString()
                }
                c < 0x20 -> throw BAD
                c == '\\'.code -> {
                    if (i > runStart) sb.append(String(b, runStart, i - runStart, Charsets.UTF_8))
                    i++
                    if (i >= end) throw BAD
                    when (b[i].toInt().toChar()) {
                        '"' -> sb.append('"'); '\\' -> sb.append('\\'); '/' -> sb.append('/')
                        'b' -> sb.append('\b'); 'f' -> sb.append('\u000C'); 'n' -> sb.append('\n')
                        'r' -> sb.append('\r'); 't' -> sb.append('\t')
                        'u' -> {
                            if (end - i < 5) throw BAD
                            var v = 0
                            for (k in 1..4) v = (v shl 4) or hex(b[i + k].toInt())
                            sb.append(v.toChar())
                            i += 4
                        }
                        else -> throw BAD
                    }
                    i++
                    runStart = i
                }
                else -> i++
            }
        }
    }

    private fun hex(c: Int): Int = when (c) {
        in '0'.code..'9'.code -> c - '0'.code
        in 'a'.code..'f'.code -> c - 'a'.code + 10
        in 'A'.code..'F'.code -> c - 'A'.code + 10
        else -> throw BAD
    }
}
