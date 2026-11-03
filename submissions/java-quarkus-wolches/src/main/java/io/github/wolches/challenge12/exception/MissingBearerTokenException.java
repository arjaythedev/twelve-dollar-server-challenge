package io.github.wolches.challenge12.exception;

public final class MissingBearerTokenException extends BusinessLogicException {
    public static final String MSG = "missing bearer token";
    public MissingBearerTokenException() {
        super(MSG);
    }

    @Override
    public int getStatus() {
        return 401;
    }
}
