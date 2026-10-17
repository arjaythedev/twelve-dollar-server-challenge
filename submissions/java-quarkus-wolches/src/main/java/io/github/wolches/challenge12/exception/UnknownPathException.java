package io.github.wolches.challenge12.exception;

public final class UnknownPathException extends BusinessLogicException {
    public static String MSG = "not found";

    public UnknownPathException() {
        super(MSG);
    }

    @Override
    public int getStatus() {
        return 404;
    }
}
