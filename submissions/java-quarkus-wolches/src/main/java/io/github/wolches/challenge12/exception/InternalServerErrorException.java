package io.github.wolches.challenge12.exception;

public final class InternalServerErrorException extends BusinessLogicException {
    public static final String MSG = "internal server error";
    public InternalServerErrorException() {
        super(MSG);
    }

    @Override
    public int getStatus() {
        return 500;
    }
}
