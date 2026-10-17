package io.github.wolches.challenge12.exception;

public final class PostNotFoundException extends BusinessLogicException {
    public PostNotFoundException() {
        super("post not found");
    }

    @Override
    public int getStatus() {
        return 404;
    }
}
