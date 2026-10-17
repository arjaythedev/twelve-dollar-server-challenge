package com.brocchirodrigo.feed.infrastructure.persistence.sqlite;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.SegmentAllocator;
import java.lang.foreign.ValueLayout;
import java.nio.charset.StandardCharsets;

public final class SqliteStatement implements AutoCloseable {

    private static final int INITIAL_TEXT_BUFFER = 1024;

    private final SqliteConnection connection;
    private final MemorySegment handle;
    private byte[] textBuffer = new byte[INITIAL_TEXT_BUFFER];

    SqliteStatement(SqliteConnection connection, MemorySegment handle) {
        this.connection = connection;
        this.handle = handle;
    }

    public void bindLong(int index, long value) {
        connection.check(Sqlite3.bindLong(handle, index, value));
    }

    public void bindText(int index, String value, SegmentAllocator allocator) {
        var text = allocator.allocateFrom(value);
        connection.check(Sqlite3.bindText(handle, index, text, Math.toIntExact(text.byteSize() - 1)));
    }

    public boolean step() {
        return switch (Sqlite3.step(handle)) {
            case Sqlite3.ROW -> true;
            case Sqlite3.DONE -> false;
            default -> throw new SqliteException(connection.errorMessage());
        };
    }

    public long getLong(int column) {
        return Sqlite3.columnLong(handle, column);
    }

    @SuppressWarnings("restricted")
    public String getText(int column) {
        var text = Sqlite3.columnText(handle, column);
        var length = Sqlite3.columnBytes(handle, column);
        if (textBuffer.length < length) {
            textBuffer = new byte[Math.max(length, textBuffer.length * 2)];
        }
        MemorySegment.copy(text.reinterpret(length), ValueLayout.JAVA_BYTE, 0, textBuffer, 0, length);
        return new String(textBuffer, 0, length, StandardCharsets.UTF_8);
    }

    public void reset() {
        Sqlite3.reset(handle);
    }

    @Override
    public void close() {
        Sqlite3.finalizeStatement(handle);
    }
}
