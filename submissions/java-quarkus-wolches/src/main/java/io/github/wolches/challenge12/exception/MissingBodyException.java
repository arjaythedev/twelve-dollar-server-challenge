package io.github.wolches.challenge12.exception;

public final class MissingBodyException extends BusinessLogicException {
    public MissingBodyException() {
        super("body is required");
    }

    @Override
    public int getStatus() {
        return 400;
    }
}
