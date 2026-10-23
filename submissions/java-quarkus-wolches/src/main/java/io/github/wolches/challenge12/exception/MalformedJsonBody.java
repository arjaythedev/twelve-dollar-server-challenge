package io.github.wolches.challenge12.exception;

public final class MalformedJsonBody extends BusinessLogicException {
    public static final String MSG = "malformed JSON body";
    public MalformedJsonBody() {
        super(MSG);
    }

    @Override
    public int getStatus() {
        return 400;
    }
}
