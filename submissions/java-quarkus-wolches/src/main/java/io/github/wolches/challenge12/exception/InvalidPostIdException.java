package io.github.wolches.challenge12.exception;

public final class InvalidPostIdException extends BusinessLogicException{

    public InvalidPostIdException() {
        super("invalid post id");
    }

    @Override
    public int getStatus() {
        return 400;
    }
}
