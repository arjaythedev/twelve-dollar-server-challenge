package dev.relism.benchmark.http;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;

/** Byte utilities only; SQLite validates and decodes JSON documents. */
public final class Json {
    private Json() {}

    public static byte[] error(String message) {
        var out = new ByteArrayOutputStream(80);
        out.writeBytes("{\"error\":\"".getBytes(StandardCharsets.US_ASCII));
        for (byte b : message.getBytes(StandardCharsets.UTF_8)) {
            int c = b & 255;
            if (c == '"' || c == '\\') { out.write('\\'); out.write(c); }
            else if (c < 32) {
                out.writeBytes(new byte[]{'\\','u','0','0', (byte) Character.forDigit(c >>> 4, 16),
                        (byte) Character.forDigit(c & 15, 16)});
            } else out.write(c);
        }
        out.write('"'); out.write('}');
        return out.toByteArray();
    }

    public static long positiveId(CharSequence text) {
        if (text == null || text.isEmpty()) return -1;
        long value = 0;
        for (int i = 0; i < text.length(); i++) {
            int digit = text.charAt(i) - '0';
            if (digit < 0 || digit > 9 || value > (Long.MAX_VALUE - digit) / 10) return -1;
            value = value * 10 + digit;
        }
        return value > 0 ? value : -1;
    }

    public static long positiveId(byte[] text) {
        if (text == null || text.length == 0) return -1;
        long value = 0;
        for (byte b : text) {
            int digit = b - '0';
            if (digit < 0 || digit > 9 || value > (Long.MAX_VALUE - digit) / 10) return -1;
            value = value * 10 + digit;
        }
        return value > 0 ? value : -1;
    }

    /** Counts scalar values, rejecting overlong sequences, surrogates and values above U+10FFFF. */
    public static int utf8Length(byte[] bytes) {
        int count = 0;
        for (int i = 0; i < bytes.length; count++) {
            int b = bytes[i++] & 255;
            if (b < 128) continue;
            int length, value, minimum;
            if (b >= 0xc2 && b <= 0xdf) { length = 1; value = b & 31; minimum = 0x80; }
            else if (b >= 0xe0 && b <= 0xef) { length = 2; value = b & 15; minimum = 0x800; }
            else if (b >= 0xf0 && b <= 0xf4) { length = 3; value = b & 7; minimum = 0x10000; }
            else return -1;
            if (i + length > bytes.length) return -1;
            for (int j = 0; j < length; j++) {
                int next = bytes[i++] & 255;
                if ((next & 0xc0) != 0x80) return -1;
                value = (value << 6) | (next & 63);
            }
            if (value < minimum || value > 0x10ffff || value >= 0xd800 && value <= 0xdfff) return -1;
        }
        return count;
    }
}
