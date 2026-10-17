package io.github.wolches.challenge12.exception;

public final class InvalidTokenPayloadException extends BusinessLogicException {
    public static final String MSG = "invalid token payload";
    public InvalidTokenPayloadException() {
        super(MSG);
    }

    @Override
    public int getStatus() {
        return 401;
    }
}
