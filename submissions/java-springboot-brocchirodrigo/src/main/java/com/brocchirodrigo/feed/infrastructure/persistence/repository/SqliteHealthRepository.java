package com.brocchirodrigo.feed.infrastructure.persistence.repository;

import com.brocchirodrigo.feed.domain.repository.HealthRepository;
import com.brocchirodrigo.feed.infrastructure.persistence.sqlite.SqliteConnection;
import com.brocchirodrigo.feed.infrastructure.persistence.sqlite.SqliteDatabase;
import com.brocchirodrigo.feed.infrastructure.persistence.sqlite.SqliteStatement;

public final class SqliteHealthRepository implements HealthRepository, AutoCloseable {

    private final Object lock = new Object();
    private final SqliteConnection connection;
    private final SqliteStatement pingStatement;

    public SqliteHealthRepository(SqliteDatabase database) {
        this.connection = database.open();
        this.pingStatement = connection.prepare("SELECT 1");
    }

    @Override
    public void ping() {
        synchronized (lock) {
            try {
                pingStatement.step();
            } finally {
                pingStatement.reset();
            }
        }
    }

    @Override
    public void close() {
        synchronized (lock) {
            pingStatement.close();
            connection.close();
        }
    }
}
