package app;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;

/** Dispatch on the raw path bytes: no string, no map, no regex. */
final class Router {
    private Router() {}

    private static final byte[] P_FEED = ascii("/feed");
    private static final byte[] P_HEALTH = ascii("/health");
    private static final byte[] P_POSTS = ascii("/posts");
    private static final byte[] P_LIKE = ascii("/like");

    private static byte[] ascii(String s) { return s.getBytes(StandardCharsets.US_ASCII); }

    static int dispatch(Http.Req r, Out out) {
        ByteBuffer b = r.buf;
        int off = r.pathOff, len = r.pathLen, end = off + len;

        if (r.method == Http.M_GET) {
            if (eq(b, off, len, P_FEED)) return Handlers.feed(out);
            if (eq(b, off, len, P_HEALTH)) return Handlers.health(out);
            if (isPostsPrefix(b, off, len)) {
                int segOff = off + P_POSTS.length + 1;
                if (segOff < end && indexOf(b, segOff, end, (byte) '/') < 0) {
                    return Handlers.post(r, segOff, end - segOff, out);
                }
            }
            return Handlers.notFound(out);
        }
        if (r.method == Http.M_POST) {
            if (eq(b, off, len, P_POSTS)) return Handlers.create(r, out);
            if (isPostsPrefix(b, off, len)) {
                int segOff = off + P_POSTS.length + 1;
                int slash = indexOf(b, segOff, end, (byte) '/');
                if (slash > segOff && eq(b, slash, end - slash, P_LIKE)) {
                    return Handlers.like(r, segOff, slash - segOff, out);
                }
            }
            return Handlers.notFound(out);
        }
        return Handlers.notFound(out);
    }

    /** True for "/posts/..." (prefix plus at least the slash). */
    private static boolean isPostsPrefix(ByteBuffer b, int off, int len) {
        if (len <= P_POSTS.length) return false;
        for (int i = 0; i < P_POSTS.length; i++) if (b.get(off + i) != P_POSTS[i]) return false;
        return b.get(off + P_POSTS.length) == '/';
    }

    private static boolean eq(ByteBuffer b, int off, int len, byte[] lit) {
        if (len != lit.length) return false;
        for (int i = 0; i < len; i++) if (b.get(off + i) != lit[i]) return false;
        return true;
    }

    private static int indexOf(ByteBuffer b, int from, int to, byte v) {
        for (int i = from; i < to; i++) if (b.get(i) == v) return i;
        return -1;
    }
}
