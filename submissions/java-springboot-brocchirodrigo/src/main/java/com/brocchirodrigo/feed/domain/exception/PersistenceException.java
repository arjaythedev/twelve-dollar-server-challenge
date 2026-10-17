package com.brocchirodrigo.feed.domain.exception;

import java.io.Serial;

public class PersistenceException extends RuntimeException {

    @Serial
    private static final long serialVersionUID = 1L;

    public PersistenceException(String message) {
        super(message);
    }
}
