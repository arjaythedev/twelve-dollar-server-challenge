package io.github.wolches.challenge12.model;

import io.github.wolches.challenge12.exception.InvalidPostIdException;

import java.util.regex.Pattern;

public record PostId(int value) {

    private static final Pattern POSITIVE = Pattern.compile("[1-9]\\d*");

    public static PostId fromString(String raw) {
        if (raw == null || !POSITIVE.matcher(raw).matches()) {
            throw new InvalidPostIdException();
        }
        try {
            return new PostId(Integer.parseInt(raw));
        } catch (NumberFormatException e) {
            throw new InvalidPostIdException();
        }
    }
}