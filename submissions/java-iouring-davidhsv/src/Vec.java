import java.nio.ByteOrder;
import jdk.incubator.vector.ByteVector;
import jdk.incubator.vector.VectorMask;
import jdk.incubator.vector.VectorOperators;
import jdk.incubator.vector.VectorSpecies;

/** Out.copyCheck for 32 bytes and more, a vector register at a time: 256-bit vectors where the
 * machine has them (not 512: on the box's Cascade Lake a 512-bit instruction lowers the core's
 * clock for the code that follows), else the widest it has. */
final class Vec {
    private Vec() {}

    private static final VectorSpecies<Byte> S =
            ByteVector.SPECIES_PREFERRED.length() > 32 ? ByteVector.SPECIES_256 : ByteVector.SPECIES_PREFERRED;
    private static final int W = S.length();
    private static final ByteOrder ORDER = ByteOrder.nativeOrder();

    /** Copies the n (>= W) bytes at from to to and says whether any of them needs escaping in a
     * JSON string: nonzero if one does. The last vector overlaps the one before it. */
    static long copyCheck(long from, long to, long n) {
        long last = n - W;
        long bad = 0;
        for (long i = 0; ; i += W) {
            if (i > last) i = last;
            ByteVector v = ByteVector.fromMemorySegment(S, N.M, from + i, ORDER);
            v.intoMemorySegment(N.M, to + i, ORDER);
            VectorMask<Byte> m = v.compare(VectorOperators.ULT, (byte) 0x20)
                    .or(v.compare(VectorOperators.EQ, (byte) '"'))
                    .or(v.compare(VectorOperators.EQ, (byte) '\\'));
            if (m.anyTrue()) bad = 1;
            if (i == last) return bad;
        }
    }

    static int width() { return W; }
}
