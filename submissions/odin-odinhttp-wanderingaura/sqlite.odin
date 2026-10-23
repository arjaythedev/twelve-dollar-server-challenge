package main

import "core:c"

// The pinned SQLite amalgamation, compiled by build.sh.
foreign import sqlite {".deps/libsqlite3.a", "system:m"}

Sqlite3      :: distinct rawptr
Sqlite3_Stmt :: distinct rawptr

SQLITE_OK   :: 0
SQLITE_ROW  :: 100
SQLITE_DONE :: 101

SQLITE_OPEN_READWRITE :: 0x00000002

SQLITE_CONFIG_PAGECACHE    :: 7
SQLITE_CONFIG_PCACHE_HDRSZ :: 24

SQLITE_PREPARE_PERSISTENT :: 0x01

// Tells SQLite to copy bound text: it is (void*)-1 in C.
SQLITE_TRANSIENT :: rawptr(~uintptr(0))

@(default_calling_convention = "c", link_prefix = "sqlite3_")
foreign sqlite {
	config         :: proc(op: c.int, #c_vararg args: ..any) -> c.int ---
	initialize     :: proc() -> c.int ---
	open_v2        :: proc(filename: cstring, db: ^Sqlite3, flags: c.int, vfs: cstring) -> c.int ---
	exec           :: proc(db: Sqlite3, sql: cstring, cb: rawptr, arg: rawptr, errmsg: ^cstring) -> c.int ---
	prepare_v3     :: proc(db: Sqlite3, sql: cstring, n: c.int, flags: c.uint, stmt: ^Sqlite3_Stmt, tail: ^cstring) -> c.int ---
	step           :: proc(stmt: Sqlite3_Stmt) -> c.int ---
	reset          :: proc(stmt: Sqlite3_Stmt) -> c.int ---
	finalize       :: proc(stmt: Sqlite3_Stmt) -> c.int ---
	bind_int64     :: proc(stmt: Sqlite3_Stmt, idx: c.int, v: i64) -> c.int ---
	bind_text      :: proc(stmt: Sqlite3_Stmt, idx: c.int, s: [^]byte, n: c.int, destructor: rawptr) -> c.int ---
	column_int64   :: proc(stmt: Sqlite3_Stmt, col: c.int) -> i64 ---
	column_text    :: proc(stmt: Sqlite3_Stmt, col: c.int) -> [^]byte ---
	column_bytes   :: proc(stmt: Sqlite3_Stmt, col: c.int) -> c.int ---
	changes        :: proc(db: Sqlite3) -> c.int ---
	errmsg         :: proc(db: Sqlite3) -> cstring ---
}

column_string :: #force_inline proc(stmt: Sqlite3_Stmt, col: c.int) -> []byte {
	p := column_text(stmt, col)
	return p[:column_bytes(stmt, col)]
}
