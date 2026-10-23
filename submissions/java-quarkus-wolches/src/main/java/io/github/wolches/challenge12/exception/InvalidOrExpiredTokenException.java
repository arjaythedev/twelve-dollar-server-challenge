package io.github.wolches.challenge12.exception;

public final class InvalidOrExpiredTokenException extends BusinessLogicException {
    public static final String MSG = "invalid or expired token";
    public InvalidOrExpiredTokenException() {
        super(MSG);
    }

    @Override
    public int getStatus() {
        return 401;
    }
}
