package com.brocchirodrigo.feed.domain.model;

public final class PositiveId {

    public static final long INVALID = 0;

    private static final int MAX_DIGITS = 18;

    private PositiveId() {
    }

    public static long parse(String text) {
        if (text == null || text.isEmpty() || text.length() > MAX_DIGITS) {
            return INVALID;
        }
        var first = text.charAt(0) - '0';
        if (first < 1 || first > 9) {
            return INVALID;
        }
        long id = first;
        for (var i = 1; i < text.length(); i++) {
            var digit = text.charAt(i) - '0';
            if (digit < 0 || digit > 9) {
                return INVALID;
            }
            id = id * 10 + digit;
        }
        return id;
    }
}
