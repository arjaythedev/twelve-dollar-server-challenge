package io.github.wolches.challenge12.exception;

public final class TooLongBodyException extends BusinessLogicException {
    public TooLongBodyException() {
        super("body must be at most 500 characters");
    }

    @Override
    public int getStatus() {
        return 400;
    }
}
