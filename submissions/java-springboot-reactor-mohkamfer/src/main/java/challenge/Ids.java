package challenge;

/** Parsing for the decimal ids that arrive in request paths and JWT subjects. */
final class Ids {

    private Ids() {}

    /** Parses a strictly positive decimal integer (digits only, no sign); -1 if it isn't one. */
    static long parsePositiveLong(String text) {
        int n = text.length();
        if (n == 0 || n > 18) return -1;
        long v = 0;
        for (int k = 0; k < n; k++) {
            char c = text.charAt(k);
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v > 0 ? v : -1;
    }
}
