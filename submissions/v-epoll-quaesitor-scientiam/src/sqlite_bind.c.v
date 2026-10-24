module main

// The SQLite amalgamation is compiled once by build.sh (with its own -D flags and PGO)
// into build/sqlite3.o; only the handful of C entry points used by db.v are declared here.
#flag -I@VMODROOT/build
#flag @VMODROOT/build/sqlite3.o
#flag -lm
#include "sqlite3.h"

struct C.sqlite3 {}

struct C.sqlite3_stmt {}

fn C.sqlite3_open_v2(&char, &&C.sqlite3, int, &char) int
fn C.sqlite3_exec(&C.sqlite3, &char, voidptr, voidptr, &&char) int
fn C.sqlite3_prepare_v3(&C.sqlite3, &char, int, u32, &&C.sqlite3_stmt, &&char) int
fn C.sqlite3_bind_int64(&C.sqlite3_stmt, int, i64) int
fn C.sqlite3_bind_text(&C.sqlite3_stmt, int, &char, int, voidptr) int
fn C.sqlite3_step(&C.sqlite3_stmt) int
fn C.sqlite3_reset(&C.sqlite3_stmt) int
fn C.sqlite3_column_int64(&C.sqlite3_stmt, int) i64
fn C.sqlite3_column_text(&C.sqlite3_stmt, int) &u8
fn C.sqlite3_column_bytes(&C.sqlite3_stmt, int) int
fn C.sqlite3_changes(&C.sqlite3) int
fn C.sqlite3_last_insert_rowid(&C.sqlite3) i64
fn C.sqlite3_errmsg(&C.sqlite3) &char
fn C.sqlite3_wal_hook(&C.sqlite3, voidptr, voidptr) voidptr
fn C.sqlite3_wal_checkpoint_v2(&C.sqlite3, &char, int, &int, &int) int
fn C.sqlite3_close(&C.sqlite3) int

const sqlite_ok = 0
const sqlite_row = 100
const sqlite_done = 101
const sqlite_open_readwrite = 0x00000002
const sqlite_open_nomutex = 0x00008000
const sqlite_prepare_persistent = u32(0x01)
const sqlite_checkpoint_passive = 0
const sqlite_checkpoint_full = 1
