package io.github.wolches.challenge12.exception;

public abstract class BusinessLogicException extends RuntimeException {
    public BusinessLogicException(String message) {
        super(message);
    }

    public abstract int getStatus();
}
