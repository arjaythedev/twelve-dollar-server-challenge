//! The few SQLite C functions the server calls, declared by hand.

pub const Db = opaque {};
pub const Stmt = opaque {};

pub const OK = 0;
pub const ROW = 100;
pub const DONE = 101;
pub const OPEN_READWRITE = 0x00000002;
pub const OPEN_NOMUTEX = 0x00008000;
pub const PREPARE_PERSISTENT = 0x01;
pub const CHECKPOINT_PASSIVE = 0;
pub const STATIC: ?*const fn (?*anyopaque) callconv(.c) void = null;

pub extern fn sqlite3_initialize() c_int;
pub extern fn sqlite3_open_v2(filename: [*:0]const u8, db: *?*Db, flags: c_int, vfs: ?[*:0]const u8) c_int;
pub extern fn sqlite3_close(db: *Db) c_int;
pub extern fn sqlite3_errmsg(db: *Db) [*:0]const u8;
pub extern fn sqlite3_exec(db: *Db, sql: [*:0]const u8, callback: ?*const anyopaque, arg: ?*anyopaque, errmsg: ?*?[*:0]u8) c_int;
pub extern fn sqlite3_prepare_v3(db: *Db, sql: [*]const u8, len: c_int, flags: c_uint, stmt: *?*Stmt, tail: ?*?[*]const u8) c_int;
pub extern fn sqlite3_step(stmt: *Stmt) c_int;
pub extern fn sqlite3_reset(stmt: *Stmt) c_int;
pub extern fn sqlite3_bind_int64(stmt: *Stmt, index: c_int, value: i64) c_int;
pub extern fn sqlite3_bind_text(stmt: *Stmt, index: c_int, text: [*]const u8, len: c_int, destructor: ?*const fn (?*anyopaque) callconv(.c) void) c_int;
pub extern fn sqlite3_column_int64(stmt: *Stmt, col: c_int) i64;
pub extern fn sqlite3_column_text(stmt: *Stmt, col: c_int) ?[*]const u8;
pub extern fn sqlite3_column_bytes(stmt: *Stmt, col: c_int) c_int;
pub extern fn sqlite3_changes(db: *Db) c_int;
pub extern fn sqlite3_wal_hook(db: *Db, callback: *const fn (?*anyopaque, *Db, [*:0]const u8, c_int) callconv(.c) c_int, arg: ?*anyopaque) ?*anyopaque;
pub extern fn sqlite3_wal_checkpoint_v2(db: *Db, name: ?[*:0]const u8, mode: c_int, log: ?*c_int, ckpt: ?*c_int) c_int;

pub fn columnText(stmt: *Stmt, col: c_int) []const u8 {
    const ptr = sqlite3_column_text(stmt, col) orelse return "";
    return ptr[0..@intCast(sqlite3_column_bytes(stmt, col))];
}
