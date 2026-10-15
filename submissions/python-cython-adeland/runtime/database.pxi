# Statements cache plans only; each request executes fresh SQL.

cdef int reset_statement(sqlite3_stmt* stmt) noexcept nogil:
    cdef int rc = sqlite3_reset(stmt)
    sqlite3_clear_bindings(stmt)
    return rc

cdef int prepare(sqlite3* db, const char* sql, sqlite3_stmt** stmt) noexcept nogil:
    return sqlite3_prepare_v2(db,sql,-1,stmt,NULL)

cdef void open_database(Server* s, const char* path) except *:
    if sqlite3_open_v2(path,&s.db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOMUTEX,NULL) != SQLITE_OK:
        raise RuntimeError('cannot open SQLite database')
    if sqlite3_exec(s.db,
        "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;"
        "PRAGMA cache_size=-32768;PRAGMA mmap_size=268435456;"
        "PRAGMA temp_store=MEMORY;PRAGMA busy_timeout=0;", NULL, NULL, NULL
    ) != SQLITE_OK:
        raise RuntimeError('cannot configure SQLite')
    if (
        prepare(s.db,
            "SELECT p.id,p.body,p.created_at,u.username,"
            "(SELECT count(*) FROM likes l WHERE l.post_id=p.id) "
            "FROM posts p JOIN users u ON u.id=p.user_id "
            "ORDER BY p.created_at DESC,p.id DESC LIMIT 20", &s.feed) != SQLITE_OK
        or prepare(s.db,
            "SELECT p.id,p.body,p.created_at,u.username,"
            "(SELECT count(*) FROM likes l WHERE l.post_id=p.id) "
            "FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?", &s.post) != SQLITE_OK
        or prepare(s.db,
            "INSERT INTO posts(user_id,body) VALUES(?,?) RETURNING id,created_at", &s.create) != SQLITE_OK
        or prepare(s.db,
            "INSERT INTO likes(user_id,post_id) SELECT ?1,?2 "
            "WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) "
            "ON CONFLICT(user_id,post_id) DO NOTHING", &s.like) != SQLITE_OK
        or prepare(s.db, "SELECT 1 FROM posts WHERE id=?", &s.exists) != SQLITE_OK
        or prepare(s.db, "SELECT 1", &s.health) != SQLITE_OK
    ):
        raise RuntimeError('cannot prepare SQLite statements')

cdef void close_database(Server* s) noexcept nogil:
    sqlite3_finalize(s.feed)
    sqlite3_finalize(s.post)
    sqlite3_finalize(s.create)
    sqlite3_finalize(s.like)
    sqlite3_finalize(s.exists)
    sqlite3_finalize(s.health)
    if s.db != NULL: sqlite3_close(s.db)
