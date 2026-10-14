package com.brocchirodrigo.feed.infrastructure.persistence.sqlite;

import java.util.List;

public final class SqliteDatabase {

    private static final List<String> PRAGMAS = List.of(
            "PRAGMA journal_mode = WAL",
            "PRAGMA synchronous = NORMAL",
            "PRAGMA busy_timeout = 5000",
            "PRAGMA cache_size = -65536",
            "PRAGMA mmap_size = 1073741824",
            "PRAGMA temp_store = MEMORY");

    private final String path;

    public SqliteDatabase(String path) {
        this.path = path;
    }

    public SqliteConnection open() {
        var connection = SqliteConnection.open(path);
        PRAGMAS.forEach(connection::execute);
        return connection;
    }
}
