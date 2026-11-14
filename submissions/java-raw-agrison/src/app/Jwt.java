package app;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.Base64;

/**
 * HS256 verification, done from scratch on every single request (rule 5: nothing about a token is
 * ever remembered). The HMAC is taken over the "header.payload" bytes as they sit in the request
 * buffer, and the payload is decoded into the request's own native scratch so the username can be
 * handed to SQLite as a pointer.
 */
final class Jwt {
    private Jwt() {}

    static final int OK = 0, MISSING = 1, INVALID = 2, BAD_PAYLOAD = 3;

    private static final byte[] BEARER = {'B', 'e', 'a', 'r', 'e', 'r', ' '};
    private static final byte[] HS256 = {'H', 'S', '2', '5', '6'};
    private static final byte[] ENC =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_".getBytes(StandardCharsets.ISO_8859_1);
    private static final byte[] DEC = new byte[128];
    static {
        for (int i = 0; i < DEC.length; i++) DEC[i] = -1;
        for (int i = 0; i < ENC.length; i++) DEC[ENC[i]] = (byte) i;
    }
    /** The header every library emits for HS256; a byte compare short-circuits the usual case. */
    private static final byte[] STD_HEADER = Base64.getUrlEncoder().withoutPadding()
            .encode("{\"alg\":\"HS256\",\"typ\":\"JWT\"}".getBytes(StandardCharsets.US_ASCII));

    private static final long MAX_USER_ID = (1L << 53);

    /**
     * Verifies the Authorization header. On OK, out.userId and out.nameOff/nameLen are set, the
     * username bytes living inside out's buffer.
     */
    static int verify(Http.Req r, Out out, Ctx c, long nowSec) {
        ByteBuffer b = r.buf;
        int off = r.authOff, len = r.authLen;
        if (off < 0 || len < BEARER.length) return MISSING;
        for (int i = 0; i < BEARER.length; i++) if (b.get(off + i) != BEARER[i]) return MISSING;

        int tok = off + BEARER.length, tokEnd = off + len, n = tokEnd - tok;
        if (n < 5) return INVALID;

        // Exactly three base64url segments.
        int d1 = -1, d2 = -1;
        for (int i = tok; i < tokEnd; i++) {
            byte ch = b.get(i);
            if (ch == '.') {
                if (d1 < 0) d1 = i;
                else if (d2 < 0) d2 = i;
                else return INVALID;
            } else if ((ch & 0x80) != 0 || DEC[ch] < 0) {
                return INVALID;
            }
        }
        if (d1 < 0 || d2 < 0) return INVALID;
        int hLen = d1 - tok, pLen = d2 - d1 - 1, sLen = tokEnd - d2 - 1;
        if (hLen == 0 || pLen == 0 || sLen != 43) return INVALID;   // 43 chars = a 32-byte HMAC

        // Header: alg must be HS256.
        if (!stdHeader(b, tok, hLen)) {
            int hn = b64decode(b, tok, hLen, out.seg, Out.JWT_OFF, Out.JWT_CAP);
            if (hn < 0) return INVALID;
            JsonScan js = c.json;
            if (!js.parseObject(out.seg, Out.JWT_OFF, hn)) return INVALID;
            if (!js.algString || js.algEsc || js.algLen != HS256.length) return INVALID;
            for (int i = 0; i < HS256.length; i++) {
                if (out.seg.get(ValueLayout.JAVA_BYTE, js.algOff + i) != HS256[i]) return INVALID;
            }
        }

        // Signature over the raw "header.payload" bytes.
        int signedLen = d2 - tok;
        if (signedLen > c.macIn.length) return INVALID;
        b.get(tok, c.macIn, 0, signedLen);
        try {
            c.mac.update(c.macIn, 0, signedLen);
            c.mac.doFinal(c.macOut, 0);
        } catch (javax.crypto.ShortBufferException e) {
            throw new IllegalStateException(e);
        }
        b64encode32(c.macOut, c.sigEnc);
        int diff = 0;
        for (int i = 0; i < 43; i++) diff |= c.sigEnc[i] ^ b.get(d2 + 1 + i);
        if (diff != 0) return INVALID;

        // Payload.
        int pn = b64decode(b, d1 + 1, pLen, out.seg, Out.JWT_OFF, Out.JWT_CAP);
        if (pn < 0) return INVALID;
        JsonScan js = c.json;
        if (!js.parseObject(out.seg, Out.JWT_OFF, pn)) return INVALID;
        if (js.expFound) {
            if (!js.expNumber) return INVALID;
            if (nowSec >= js.expVal) return INVALID;
        }

        // sub: a string of digits, > 0.
        if (!js.subFound || !js.subString) return BAD_PAYLOAD;
        long subOff = js.subOff;
        int subLen = js.subLen;
        if (js.subEsc) {
            subLen = JsonScan.decode(out.seg, subOff, subLen, out.seg, Out.NAME_OFF, Out.NAME_CAP);
            if (subLen < 0) return BAD_PAYLOAD;
            subOff = Out.NAME_OFF;
        }
        if (subLen == 0 || subLen > 16) return BAD_PAYLOAD;
        long id = 0;
        for (int i = 0; i < subLen; i++) {
            int ch = out.seg.get(ValueLayout.JAVA_BYTE, subOff + i) & 0xFF;
            if (ch < '0' || ch > '9') return BAD_PAYLOAD;
            id = id * 10 + (ch - '0');
        }
        if (id <= 0 || id > MAX_USER_ID) return BAD_PAYLOAD;

        if (!js.nameFound || !js.nameString) return BAD_PAYLOAD;
        if (js.nameEsc) {
            int dn = JsonScan.decode(out.seg, js.nameOff, js.nameLen, out.seg, Out.NAME_OFF, Out.NAME_CAP);
            if (dn < 0) return BAD_PAYLOAD;
            out.nameOff = Out.NAME_OFF;
            out.nameLen = dn;
        } else {
            out.nameOff = (int) js.nameOff;
            out.nameLen = js.nameLen;
        }
        out.userId = id;
        return OK;
    }

    private static boolean stdHeader(ByteBuffer b, int off, int len) {
        if (len != STD_HEADER.length) return false;
        for (int i = 0; i < len; i++) if (b.get(off + i) != STD_HEADER[i]) return false;
        return true;
    }

    /** base64url (no padding) from the request buffer into native memory. Returns -1 if invalid. */
    private static int b64decode(ByteBuffer b, int off, int len, MemorySegment dst, long dstOff, int dstCap) {
        if ((len & 3) == 1) return -1;
        int full = len >> 2, rem = len & 3;
        int outLen = full * 3 + (rem == 0 ? 0 : rem - 1);
        if (outLen > dstCap) return -1;
        int i = off, o = 0;
        for (int g = 0; g < full; g++) {
            int v = d(b, i) << 18 | d(b, i + 1) << 12 | d(b, i + 2) << 6 | d(b, i + 3);
            if (v < 0) return -1;
            dst.set(ValueLayout.JAVA_BYTE, dstOff + o, (byte) (v >> 16));
            dst.set(ValueLayout.JAVA_BYTE, dstOff + o + 1, (byte) (v >> 8));
            dst.set(ValueLayout.JAVA_BYTE, dstOff + o + 2, (byte) v);
            i += 4;
            o += 3;
        }
        if (rem == 2) {
            int v = d(b, i) << 18 | d(b, i + 1) << 12;
            if (v < 0) return -1;
            dst.set(ValueLayout.JAVA_BYTE, dstOff + o, (byte) (v >> 16));
            o++;
        } else if (rem == 3) {
            int v = d(b, i) << 18 | d(b, i + 1) << 12 | d(b, i + 2) << 6;
            if (v < 0) return -1;
            dst.set(ValueLayout.JAVA_BYTE, dstOff + o, (byte) (v >> 16));
            dst.set(ValueLayout.JAVA_BYTE, dstOff + o + 1, (byte) (v >> 8));
            o += 2;
        }
        return o;
    }

    private static int d(ByteBuffer b, int i) {
        byte ch = b.get(i);
        return (ch & 0x80) != 0 ? -1 : DEC[ch];
    }

    /** 32 bytes -> 43 base64url characters, no padding. */
    private static void b64encode32(byte[] in, byte[] out) {
        int o = 0, i = 0;
        for (int g = 0; g < 10; g++) {
            int v = (in[i] & 0xFF) << 16 | (in[i + 1] & 0xFF) << 8 | (in[i + 2] & 0xFF);
            out[o] = ENC[v >>> 18];
            out[o + 1] = ENC[(v >>> 12) & 63];
            out[o + 2] = ENC[(v >>> 6) & 63];
            out[o + 3] = ENC[v & 63];
            i += 3;
            o += 4;
        }
        int v = (in[30] & 0xFF) << 16 | (in[31] & 0xFF) << 8;
        out[40] = ENC[v >>> 18];
        out[41] = ENC[(v >>> 12) & 63];
        out[42] = ENC[(v >>> 6) & 63];
    }
}
