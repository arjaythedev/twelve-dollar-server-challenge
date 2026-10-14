package com.brocchirodrigo.feed.infrastructure.persistence.sqlite;

import module java.base;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

@SuppressWarnings("restricted")
final class Sqlite3 {

    static final int OK = 0;
    static final int ROW = 100;
    static final int DONE = 101;

    private static final int OPEN_READWRITE = 0x00000002;
    private static final int OPEN_NOMUTEX = 0x00008000;
    private static final int PREPARE_PERSISTENT = 0x01;

    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup LIBRARY = library("libsqlite3.so.0", "libsqlite3.dylib");
    private static final Linker.Option CRITICAL = Linker.Option.critical(false);

    private static final MethodHandle OPEN = downcall("sqlite3_open_v2",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, ADDRESS));
    private static final MethodHandle CLOSE = downcall("sqlite3_close_v2",
            FunctionDescriptor.of(JAVA_INT, ADDRESS));
    private static final MethodHandle EXEC = downcall("sqlite3_exec",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, ADDRESS, ADDRESS, ADDRESS));
    private static final MethodHandle PREPARE = downcall("sqlite3_prepare_v3",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS));
    private static final MethodHandle FINALIZE = downcall("sqlite3_finalize",
            FunctionDescriptor.of(JAVA_INT, ADDRESS));
    private static final MethodHandle STEP = downcall("sqlite3_step",
            FunctionDescriptor.of(JAVA_INT, ADDRESS));
    private static final MethodHandle RESET = downcall("sqlite3_reset",
            FunctionDescriptor.of(JAVA_INT, ADDRESS), CRITICAL);
    private static final MethodHandle BIND_INT64 = downcall("sqlite3_bind_int64",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, JAVA_LONG), CRITICAL);
    private static final MethodHandle BIND_TEXT = downcall("sqlite3_bind_text",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS), CRITICAL);
    private static final MethodHandle COLUMN_INT64 = downcall("sqlite3_column_int64",
            FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_INT), CRITICAL);
    private static final MethodHandle COLUMN_TEXT = downcall("sqlite3_column_text",
            FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT), CRITICAL);
    private static final MethodHandle COLUMN_BYTES = downcall("sqlite3_column_bytes",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT), CRITICAL);
    private static final MethodHandle CHANGES = downcall("sqlite3_changes",
            FunctionDescriptor.of(JAVA_INT, ADDRESS), CRITICAL);
    private static final MethodHandle ERRMSG = downcall("sqlite3_errmsg",
            FunctionDescriptor.of(ADDRESS, ADDRESS), CRITICAL);

    private Sqlite3() {
    }

    static int open(MemorySegment path, MemorySegment database) {
        try {
            return (int) OPEN.invokeExact(path, database, OPEN_READWRITE | OPEN_NOMUTEX, MemorySegment.NULL);
        } catch (Throwable error) {
            throw rethrow(error);
        }
    }

    static int close(MemorySegment database) {
        try {
            return (int) CLOSE.invokeExact(database);
        } catch (Throwable error) {
            throw rethrow(error);
        }
    }

    static int exec(MemorySegment database, MemorySegment sql) {
        try {
            return (int) EXEC.invokeExact(database, sql, MemorySegment.NULL, MemorySegment.NULL, MemorySegment.NULL);
        } catch (Throwable error) {
            throw rethrow(error);
        }
    }

    static int prepare(MemorySegment database, MemorySegment sql, MemorySegment statement) {
        try {
            return (int) PREPARE.invokeExact(database, sql, -1, PREPARE_PERSISTENT, statement, MemorySegment.NULL);
        } catch (Throwable error) {
            throw rethrow(error);
        }
    }

    static int finalizeStatement(MemorySegment statement) {
        try {
            return (int) FINALIZE.invokeExact(statement);
        } catch (Throwable error) {
            throw rethrow(error);
        }
    }

    static int step(MemorySegment statement) {
        try {
            return (int) STEP.invokeExact(statement);
        } catch (Throwable error) {
            throw rethrow(error);
        }
    }

    static int reset(MemorySegment statement) {
        try {
            return (int) RESET.invokeExact(statement);
        } catch (Throwable error) {
            throw rethrow(error);
        }
    }

    static int bindLong(MemorySegment statement, int index, long value) {
        try {
            return (int) BIND_INT64.invokeExact(statement, index, value);
        } catch (Throwable error) {
            throw rethrow(error);
        }
    }

    static int bindText(MemorySegment statement, int index, MemorySegment text, int length) {
        try {
            return (int) BIND_TEXT.invokeExact(statement, index, text, length, MemorySegment.NULL);
        } catch (Throwable error) {
            throw rethrow(error);
        }
    }

    static long columnLong(MemorySegment statement, int column) {
        try {
            return (long) COLUMN_INT64.invokeExact(statement, column);
        } catch (Throwable error) {
            throw rethrow(error);
        }
    }

    static MemorySegment columnText(MemorySegment statement, int column) {
        try {
            return (MemorySegment) COLUMN_TEXT.invokeExact(statement, column);
        } catch (Throwable error) {
            throw rethrow(error);
        }
    }

    static int columnBytes(MemorySegment statement, int column) {
        try {
            return (int) COLUMN_BYTES.invokeExact(statement, column);
        } catch (Throwable error) {
            throw rethrow(error);
        }
    }

    static int changes(MemorySegment database) {
        try {
            return (int) CHANGES.invokeExact(database);
        } catch (Throwable error) {
            throw rethrow(error);
        }
    }

    static String errorMessage(MemorySegment database) {
        try {
            return ((MemorySegment) ERRMSG.invokeExact(database)).reinterpret(Long.MAX_VALUE).getString(0);
        } catch (Throwable error) {
            throw rethrow(error);
        }
    }

    private static SymbolLookup library(String... names) {
        for (var name : names) {
            var lookup = lookup(name);
            if (lookup.isPresent()) {
                return lookup.get();
            }
        }
        throw new IllegalStateException("libsqlite3 not found");
    }

    private static Optional<SymbolLookup> lookup(String name) {
        try {
            return Optional.of(SymbolLookup.libraryLookup(name, Arena.global()));
        } catch (IllegalArgumentException _) {
            return Optional.empty();
        }
    }

    private static MethodHandle downcall(String name, FunctionDescriptor descriptor, Linker.Option... options) {
        return LINKER.downcallHandle(LIBRARY.find(name).orElseThrow(), descriptor, options);
    }

    private static RuntimeException rethrow(Throwable error) {
        return switch (error) {
            case RuntimeException runtime -> runtime;
            case Error fatal -> throw fatal;
            default -> new IllegalStateException(error);
        };
    }
}
