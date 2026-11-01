module sqlite;

alias sqlite3 = void;
alias sqlite3_stmt = void;
alias sqlite3_destructor_type = extern (C) void function(void*);

enum SQLITE_OK = 0;
enum SQLITE_ROW = 100;
enum SQLITE_DONE = 101;

enum SQLITE_OPEN_READWRITE = 0x00000002;
enum SQLITE_OPEN_CREATE = 0x00000004;
enum SQLITE_OPEN_NOMUTEX = 0x00008000;

private enum SQLITE_TRANSIENT = cast(sqlite3_destructor_type) -1;

extern (C)
{
    nothrow @nogc:
    int sqlite3_initialize();
    int sqlite3_shutdown();
    int sqlite3_open_v2(const(char)* filename, sqlite3** ppDb, int flags, const(char)* zVfs);
    int sqlite3_close_v2(sqlite3*);
    int sqlite3_exec(sqlite3*, const(char)* sql,
        int function(void*, int, char**, char**) callback, void* arg, char** errmsg);
    int sqlite3_prepare_v2(sqlite3*, const(char)* zSql, int nByte,
        sqlite3_stmt** ppStmt, const(char)** pzTail);
    int sqlite3_bind_text(sqlite3_stmt*, int, const(char)*, int, sqlite3_destructor_type);
    int sqlite3_bind_int64(sqlite3_stmt*, int, long);
    int sqlite3_step(sqlite3_stmt*);
    int sqlite3_reset(sqlite3_stmt*);
    int sqlite3_clear_bindings(sqlite3_stmt*);
    int sqlite3_finalize(sqlite3_stmt*);
    long sqlite3_column_int64(sqlite3_stmt*, int);
    const(ubyte)* sqlite3_column_text(sqlite3_stmt*, int);
    int sqlite3_column_bytes(sqlite3_stmt*, int);
    int sqlite3_changes(sqlite3*);
    const(char)* sqlite3_errmsg(sqlite3*);
    int sqlite3_busy_timeout(sqlite3*, int);
}

nothrow @nogc:

struct Db
{
    nothrow @nogc:
    sqlite3* handle;

    bool open(const(char)* path)
    {
        if (sqlite3_open_v2(path, &handle,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                null) != SQLITE_OK)
        {
            if (handle !is null)
                sqlite3_close_v2(handle);
            handle = null;
            return false;
        }
        return true;
    }

    void close()
    {
        if (handle !is null)
        {
            sqlite3_close_v2(handle);
            handle = null;
        }
    }

    int exec(const(char)* sql)
    {
        return sqlite3_exec(handle, sql, null, null, null);
    }

    int prepare(const(char)[] sql, sqlite3_stmt** stmt)
    {
        return sqlite3_prepare_v2(handle, sql.ptr, cast(int) sql.length, stmt, null);
    }

    const(char)* errmsg()
    {
        return sqlite3_errmsg(handle);
    }

    int busyTimeout(int milliseconds)
    {
        return sqlite3_busy_timeout(handle, milliseconds);
    }

    int changes()
    {
        return sqlite3_changes(handle);
    }
}

int bindText(sqlite3_stmt* stmt, int index, const(char)[] value)
{
    return sqlite3_bind_text(stmt, index, value.ptr, cast(int) value.length, SQLITE_TRANSIENT);
}

int bindLong(sqlite3_stmt* stmt, int index, long value)
{
    return sqlite3_bind_int64(stmt, index, value);
}

long columnLong(sqlite3_stmt* stmt, int index)
{
    return sqlite3_column_int64(stmt, index);
}

const(char)[] columnText(sqlite3_stmt* stmt, int index)
{
    auto ptr = sqlite3_column_text(stmt, index);
    if (ptr is null)
        return null;
    immutable len = sqlite3_column_bytes(stmt, index);
    return cast(const(char)[]) ptr[0 .. len];
}

void statementReset(sqlite3_stmt* stmt)
{
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
}

void statementFinalize(sqlite3_stmt* stmt)
{
    if (stmt !is null)
        sqlite3_finalize(stmt);
}
