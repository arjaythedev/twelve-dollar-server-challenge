package com.brocchirodrigo.feed.infrastructure.persistence.sqlite;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;

import static java.lang.foreign.ValueLayout.ADDRESS;

public final class SqliteConnection implements AutoCloseable {

    private final MemorySegment handle;

    private SqliteConnection(MemorySegment handle) {
        this.handle = handle;
    }

    public static SqliteConnection open(String path) {
        try (var arena = Arena.ofConfined()) {
            var out = arena.allocate(ADDRESS);
            var code = Sqlite3.open(arena.allocateFrom(path), out);
            var connection = new SqliteConnection(out.get(ADDRESS, 0));
            if (code != Sqlite3.OK) {
                var message = connection.errorMessage();
                connection.close();
                throw new SqliteException(message);
            }
            return connection;
        }
    }

    public void execute(String sql) {
        try (var arena = Arena.ofConfined()) {
            check(Sqlite3.exec(handle, arena.allocateFrom(sql)));
        }
    }

    public SqliteStatement prepare(String sql) {
        try (var arena = Arena.ofConfined()) {
            var out = arena.allocate(ADDRESS);
            check(Sqlite3.prepare(handle, arena.allocateFrom(sql), out));
            return new SqliteStatement(this, out.get(ADDRESS, 0));
        }
    }

    public int changes() {
        return Sqlite3.changes(handle);
    }

    @Override
    public void close() {
        Sqlite3.close(handle);
    }

    void check(int code) {
        if (code != Sqlite3.OK) {
            throw new SqliteException(errorMessage());
        }
    }

    String errorMessage() {
        return Sqlite3.errorMessage(handle);
    }
}
