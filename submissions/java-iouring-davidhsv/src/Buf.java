import java.lang.foreign.Arena;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;

/** A byte buffer in native memory, reused from request to request as storage only. It grows by
 * doubling when a write would not fit (rarely: each starts larger than a request needs), and
 * then its old memory is freed; an address into it is good until the next write. */
final class Buf {
    long base;
    long len;
    long cap;
    private Arena arena;

    Buf(long cap) {
        arena = Arena.ofConfined();
        base = arena.allocate(cap, 16).address();
        this.cap = cap;
    }

    void clear() { len = 0; }

    void ensure(long n) { if (len + n > cap) grow(len + n); }

    private void grow(long need) {
        long c = cap;
        while (c < need) c *= 2;
        Arena a = Arena.ofConfined();
        long nb = a.allocate(c, 16).address();
        N.copy(base, nb, len);
        arena.close();
        arena = a;
        base = nb;
        cap = c;
    }

    void put(int b) {
        if (len + 1 > cap) grow(len + 1);
        N.M.set(JAVA_BYTE, base + len, (byte) b);
        len++;
    }

    void put(byte[] s) {
        if (len + s.length > cap) grow(len + s.length);
        N.copy(s, 0, base + len, s.length);
        len += s.length;
    }

    void put(long from, long n) {
        if (len + n > cap) grow(len + n);
        N.copy(from, base + len, n);
        len += n;
    }

    /** Appends a non-negative decimal integer. */
    void num(long v) {
        ensure(20);
        len += Out.digits(base + len, v);
    }

    /** True when the buffer holds exactly the bytes of s. */
    boolean is(byte[] s) { return Out.eq(base, len, s); }
}
