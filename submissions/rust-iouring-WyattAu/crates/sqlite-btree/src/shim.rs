//! FFI declarations for SQLite's internal B-tree cursor API (amalgamation 3.54.x).
//!
//! The C shim (`csrc/shim.c`, next build window) `#include`s the unmodified
//! amalgamation and compiles these symbols in; SQLite keeps ownership of page
//! walking, WAL, and locking — this crate only drives cursors and decodes
//! record bytes (`record`).
//!
//! # Safety contract
//!
//! - Every `&BtCursor`/`&Btree` reference passed in must have been produced by
//!   the shim (`td_open`) and remain valid for the connection's lifetime.
//! - Cursor ops happen only inside the open read transaction (one per event
//!   loop pass); writes go through SQL, never through raw cursors here.
//! - `sqlite3BtreePayload` writes at most `amt` bytes into `dest`; the wrapper
//!   must size `dest >= amt`.
//!
//! Reference: `sqlite3.c` 3.54.1 (the amalgamation is the API spec).

#![allow(non_camel_case_types)]

pub type Pgno = u32;
pub type i64_t = i64;

/// Opaque. Allocated with `sqlite3BtreeCursorSize()` bytes, zeroed via
/// `sqlite3BtreeCursorZero()` before first use.
#[repr(C)]
pub struct BtCursor {
    _private: [u8; 0],
}

/// Opaque. Borrowed from the shim's single `sqlite3` connection.
#[repr(C)]
pub struct Btree {
    _private: [u8; 0],
}

/// Opaque index key info; the shim builds descending-key `KeyInfo`s for the
/// two indexes we walk (`posts_created_at_id_idx`, `likes_post_id_idx`).
#[repr(C)]
pub struct KeyInfo {
    _private: [u8; 0],
}

unsafe extern "C" {
    pub fn sqlite3BtreeCursorSize() -> i32;
    pub fn sqlite3BtreeCursorZero(cur: *mut BtCursor);
    pub fn sqlite3BtreeCursor(
        bt: *mut Btree,
        table: Pgno,
        wr_flag: i32,
        key_info: *mut KeyInfo,
        cur: *mut BtCursor,
    ) -> i32;
    pub fn sqlite3BtreeCloseCursor(cur: *mut BtCursor);
    pub fn sqlite3BtreeBeginTrans(bt: *mut Btree, wr_flag: i32, schema_version: *mut i32) -> i32;
    /// Seek to rowid `key`. `*res == 0` on exact match, nonzero if absent.
    pub fn sqlite3BtreeTableMoveto(cur: *mut BtCursor, key: i64_t, bias: i32, res: *mut i32) -> i32;
    /// Index seek against an unpacked record; `*res == 0` on exact match.
    pub fn sqlite3BtreeIndexMoveto(
        cur: *mut BtCursor,
        key: *mut UnpackedRecord,
        res: *mut i32,
    ) -> i32;
    /// Position at the first entry. `*res == 1` means table/index empty.
    pub fn sqlite3BtreeFirst(cur: *mut BtCursor, res: *mut i32) -> i32;
    /// Advance. `flags` bit 1 = do not swap pages for small moves.
    /// Returns an SQLite result code; EOF state is polled via `sqlite3BtreeEof`.
    pub fn sqlite3BtreeNext(cur: *mut BtCursor, flags: i32) -> i32;
    pub fn sqlite3BtreeEof(cur: *mut BtCursor) -> i32;
    /// Rowid of the current table-cursor entry.
    pub fn sqlite3BtreeIntegerKey(cur: *mut BtCursor) -> i64_t;
    /// Record payload size of the current entry (table or index).
    pub fn sqlite3BtreePayloadSize(cur: *mut BtCursor) -> u32;
    /// Read `amt` payload bytes at `offset` into `dest`.
    pub fn sqlite3BtreePayload(cur: *mut BtCursor, offset: u32, amt: u32, dest: *mut u8) -> i32;
}

/// Opaque unpacked record used by `sqlite3BtreeIndexMoveto`. The shim builds
/// these from Rust-provided key fields (created_at bytes + id) against the
/// index's `KeyInfo`.
#[repr(C)]
pub struct UnpackedRecord {
    _private: [u8; 0],
}

/// Result codes we surface (subset of sqlite3.h; the rest map to `Other`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Rc {
    Ok,
    Row,
    Done,
    Busy,
    Corrupt,
    Other(i32),
}

#[inline]
pub fn map_rc(rc: i32) -> Rc {
    match rc {
        0 => Rc::Ok,
        100 => Rc::Row,
        101 => Rc::Done,
        5 => Rc::Busy,
        11 => Rc::Corrupt,
        other => Rc::Other(other),
    }
}
