package com.brocchirodrigo.feed.infrastructure.persistence.sqlite;

import com.brocchirodrigo.feed.domain.exception.PersistenceException;

import java.io.Serial;

public class SqliteException extends PersistenceException {

    @Serial
    private static final long serialVersionUID = 1L;

    public SqliteException(String message) {
        super(message);
    }
}
