package kload

/**
 * Log-linear latency histogram (microseconds): 64 sub-buckets per power of two, so every
 * percentile is exact to within ~1.6 %. A fixed-size array per worker, merged at the end; no per-sample allocation.
 */
class Hist {
    val c = LongArray(SIZE)
    var n = 0L
    var sum = 0L
    var max = 0L

    fun add(us: Long) {
        val v = if (us < 0) 0 else us
        c[idx(v)]++
        n++; sum += v
        if (v > max) max = v
    }

    fun merge(o: Hist) {
        for (i in c.indices) c[i] += o.c[i]
        n += o.n; sum += o.sum
        if (o.max > max) max = o.max
    }

    /** Copy of the counts (racy read from another thread is fine for progress lines). */
    fun snapshot(): Hist { val h = Hist(); System.arraycopy(c, 0, h.c, 0, SIZE); h.n = n; h.sum = sum; h.max = max; return h }

    /** this - older, bucket by bucket. */
    fun minus(o: Hist): Hist { val h = Hist(); for (i in c.indices) h.c[i] = c[i] - o.c[i]; h.n = n - o.n; h.sum = sum - o.sum; h.max = max; return h }

    /** p in [0, 1]; returns microseconds. */
    fun pct(p: Double): Double {
        if (n <= 0) return 0.0
        val target = maxOf(1L, Math.ceil(p * n).toLong())
        var acc = 0L
        for (i in c.indices) {
            acc += c[i]
            if (acc >= target) return minOf(mid(i), max.toDouble())
        }
        return max.toDouble()
    }

    fun avg(): Double = if (n == 0L) 0.0 else sum.toDouble() / n

    companion object {
        const val SIZE = 64 * 48

        fun idx(v: Long): Int {
            if (v < 64) return v.toInt()
            val e = 63 - java.lang.Long.numberOfLeadingZeros(v)
            return ((e - 5) shl 6) + ((v ushr (e - 6)) and 63).toInt()
        }

        private fun mid(i: Int): Double {
            if (i < 64) return i.toDouble()
            val e = (i shr 6) + 5
            val lo = (64L + (i and 63)) shl (e - 6)
            return lo + (1L shl (e - 6)) / 2.0
        }
    }
}
