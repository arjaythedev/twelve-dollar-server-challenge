#![allow(non_camel_case_types)]

use std::ffi::{c_char, c_int, c_uchar, c_uint, c_void};

/// Opaque handle owned by SQLite; only ever used behind a pointer.
#[repr(C)]
pub struct sqlite3 {
    // Private and zero sized, so it can't be constructed or inspected from Rust.
    _private: [u8; 0],
}

#[repr(C)]
pub struct sqlite3_stmt {
    _private: [u8; 0],
}

pub type exec_callback =
    Option<unsafe extern "C" fn(*mut c_void, c_int, *mut *mut c_char, *mut *mut c_char) -> c_int>;

pub type sqlite3_destructor = Option<unsafe extern "C" fn(*mut c_void)>;

pub const SQLITE_OK: c_int = 0;
pub const SQLITE_TOOBIG: c_int = 18;
pub const SQLITE_MISUSE: c_int = 21;
pub const SQLITE_ROW: c_int = 100;
pub const SQLITE_DONE: c_int = 101;

pub const SQLITE_INTERNAL: c_int = 2;
pub const SQLITE_CONSTRAINT: c_int = 19;
pub const SQLITE_CONSTRAINT_FOREIGNKEY: c_int = SQLITE_CONSTRAINT | (3 << 8);

pub const SQLITE_OPEN_READWRITE: c_int = 0x0000_0002;
pub const SQLITE_OPEN_NOMUTEX: c_int = 0x0000_8000;

pub const SQLITE_PREPARE_PERSISTENT: c_uint = 0x01;

pub const SQLITE_STATIC: sqlite3_destructor = None;

unsafe extern "C" {
    pub fn sqlite3_open_v2(
        filename: *const c_char,
        db: *mut *mut sqlite3,
        flags: c_int,
        vfs: *const c_char,
    ) -> c_int;
    pub fn sqlite3_close_v2(db: *mut sqlite3) -> c_int;
    pub fn sqlite3_extended_result_codes(db: *mut sqlite3, onoff: c_int) -> c_int;
    pub fn sqlite3_exec(
        db: *mut sqlite3,
        sql: *const c_char,
        callback: exec_callback,
        arg: *mut c_void,
        errmsg: *mut *mut c_char,
    ) -> c_int;
    pub fn sqlite3_errmsg(db: *mut sqlite3) -> *const c_char;

    pub fn sqlite3_prepare_v3(
        db: *mut sqlite3,
        sql: *const c_char,
        n_byte: c_int,
        prep_flags: c_uint,
        stmt: *mut *mut sqlite3_stmt,
        tail: *mut *const c_char,
    ) -> c_int;
    pub fn sqlite3_step(stmt: *mut sqlite3_stmt) -> c_int;
    pub fn sqlite3_reset(stmt: *mut sqlite3_stmt) -> c_int;
    pub fn sqlite3_finalize(stmt: *mut sqlite3_stmt) -> c_int;
    pub fn sqlite3_changes64(db: *mut sqlite3) -> i64;
    pub fn sqlite3_db_handle(stmt: *mut sqlite3_stmt) -> *mut sqlite3;

    pub fn sqlite3_bind_int64(stmt: *mut sqlite3_stmt, idx: c_int, value: i64) -> c_int;
    pub fn sqlite3_bind_text(
        stmt: *mut sqlite3_stmt,
        idx: c_int,
        value: *const c_char,
        n: c_int,
        destructor: sqlite3_destructor,
    ) -> c_int;

    pub fn sqlite3_column_int64(stmt: *mut sqlite3_stmt, col: c_int) -> i64;
    pub fn sqlite3_column_text(stmt: *mut sqlite3_stmt, col: c_int) -> *const c_uchar;
    pub fn sqlite3_column_bytes(stmt: *mut sqlite3_stmt, col: c_int) -> c_int;
}
