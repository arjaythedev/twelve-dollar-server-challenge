use crate::stats::stderr_line;
use std::cell::Cell;
use std::ffi::{CStr, CString, c_char, c_int, c_void};
use std::mem::ManuallyDrop;
use std::ptr::{self, NonNull};
use std::time::Instant;

#[cfg(debug_assertions)]
use sys::sqlite3_stmt_busy;
use sys::{
    CONSTRAINT, DONE, ERROR, NOMEM, OK, ROW, sqlite3_bind_int64, sqlite3_bind_text,
    sqlite3_changes, sqlite3_clear_bindings, sqlite3_close, sqlite3_column_bytes,
    sqlite3_column_int64, sqlite3_column_text, sqlite3_column_type, sqlite3_db_readonly,
    sqlite3_db_status64, sqlite3_errmsg, sqlite3_finalize, sqlite3_get_autocommit, sqlite3_open_v2,
    sqlite3_prepare_v3, sqlite3_reset, sqlite3_step, sqlite3_wal_checkpoint_v2, sqlite3_wal_hook,
    twelve_feed, twelve_post, twelve_reader_reason, twelve_readers_close, twelve_readers_end,
    twelve_readers_open,
};
#[cfg(twelve_fault_inject)]
use sys::{sqlite3_commit_hook, sqlite3_set_authorizer};

const BODY_MAX: usize = 131_072;

#[repr(C)]
struct ReaderBuffer {
    ptr: *mut u8,
    cap: usize,
    len: usize,
    need: usize,
}

// The same layout as readers.c's struct twelve_buf and its static asserts.
const _: () = assert!(std::mem::size_of::<ReaderBuffer>() == 32);
const _: () = assert!(std::mem::offset_of!(ReaderBuffer, ptr) == 0);
const _: () = assert!(std::mem::offset_of!(ReaderBuffer, cap) == 8);
const _: () = assert!(std::mem::offset_of!(ReaderBuffer, len) == 16);
const _: () = assert!(std::mem::offset_of!(ReaderBuffer, need) == 24);

// twelve_readers_open's result: 0 opens the readers; 1 to 4 (a table, an index, the text encoding
// or a key allocation refused) select SQL-only reads. Any other value breaks the readers' contract.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ReaderOpen {
    Opened,
    Refused,
}

impl TryFrom<c_int> for ReaderOpen {
    type Error = c_int;

    fn try_from(value: c_int) -> Result<Self, c_int> {
        match value {
            0 => Ok(Self::Opened),
            1..=4 => Ok(Self::Refused),
            unknown => Err(unknown),
        }
    }
}

// The result codes of twelve_feed and twelve_post.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ReaderStatus {
    Ok,
    NotFound,
    Fallback,
    Space,
}

impl TryFrom<c_int> for ReaderStatus {
    type Error = c_int;

    fn try_from(value: c_int) -> Result<Self, c_int> {
        match value {
            0 => Ok(Self::Ok),
            1 => Ok(Self::NotFound),
            2 => Ok(Self::Fallback),
            3 => Ok(Self::Space),
            unknown => Err(unknown),
        }
    }
}

// twelve_reader_reason's codes: None after every result but a fallback, one of the six otherwise.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ReaderReason {
    None,
    Txn,
    Cookie,
    Cursor,
    Record,
    Span,
    Missing,
}

impl TryFrom<c_int> for ReaderReason {
    type Error = c_int;

    fn try_from(value: c_int) -> Result<Self, c_int> {
        match value {
            0 => Ok(Self::None),
            1 => Ok(Self::Txn),
            2 => Ok(Self::Cookie),
            3 => Ok(Self::Cursor),
            4 => Ok(Self::Record),
            5 => Ok(Self::Span),
            6 => Ok(Self::Missing),
            unknown => Err(unknown),
        }
    }
}

// The hand-written mirror of the sqlite3.h result codes and functions Store uses and of the readers'
// entry points; only Store and its fault module use these items. bindgen is outside the pinned
// toolchain, so each function is called once by a test instead of a regeneration diff.
mod sys {
    use super::ReaderBuffer;
    use std::ffi::{c_char, c_int, c_uint, c_void};

    pub(super) const OK: c_int = 0;
    pub(super) const ERROR: c_int = 1;
    pub(super) const NOMEM: c_int = 7;
    pub(super) const CONSTRAINT: c_int = 19;
    pub(super) const ROW: c_int = 100;
    pub(super) const DONE: c_int = 101;

    #[repr(C)]
    pub(super) struct sqlite3 {
        _opaque: [u8; 0],
    }

    #[repr(C)]
    pub(super) struct sqlite3_stmt {
        _opaque: [u8; 0],
    }

    unsafe extern "C" {
        pub(super) fn sqlite3_open_v2(
            path: *const c_char,
            db: *mut *mut sqlite3,
            flags: c_int,
            vfs: *const c_char,
        ) -> c_int;
        pub(super) fn sqlite3_close(db: *mut sqlite3) -> c_int;
        pub(super) fn sqlite3_db_readonly(db: *mut sqlite3, name: *const c_char) -> c_int;
        pub(super) fn sqlite3_prepare_v3(
            db: *mut sqlite3,
            sql: *const c_char,
            len: c_int,
            flags: c_uint,
            stmt: *mut *mut sqlite3_stmt,
            tail: *mut *const c_char,
        ) -> c_int;
        pub(super) fn sqlite3_step(stmt: *mut sqlite3_stmt) -> c_int;
        #[cfg(debug_assertions)]
        pub(super) fn sqlite3_stmt_busy(stmt: *mut sqlite3_stmt) -> c_int;
        pub(super) fn sqlite3_reset(stmt: *mut sqlite3_stmt) -> c_int;
        pub(super) fn sqlite3_clear_bindings(stmt: *mut sqlite3_stmt) -> c_int;
        pub(super) fn sqlite3_finalize(stmt: *mut sqlite3_stmt) -> c_int;
        pub(super) fn sqlite3_bind_int64(stmt: *mut sqlite3_stmt, at: c_int, value: i64) -> c_int;
        pub(super) fn sqlite3_bind_text(
            stmt: *mut sqlite3_stmt,
            at: c_int,
            value: *const c_char,
            len: c_int,
            destructor: Option<unsafe extern "C" fn(*mut c_void)>,
        ) -> c_int;
        pub(super) fn sqlite3_column_int64(stmt: *mut sqlite3_stmt, at: c_int) -> i64;
        pub(super) fn sqlite3_column_type(stmt: *mut sqlite3_stmt, at: c_int) -> c_int;
        pub(super) fn sqlite3_column_text(stmt: *mut sqlite3_stmt, at: c_int) -> *const u8;
        pub(super) fn sqlite3_column_bytes(stmt: *mut sqlite3_stmt, at: c_int) -> c_int;
        pub(super) fn sqlite3_get_autocommit(db: *mut sqlite3) -> c_int;
        pub(super) fn sqlite3_changes(db: *mut sqlite3) -> c_int;
        pub(super) fn sqlite3_errmsg(db: *mut sqlite3) -> *const c_char;
        pub(super) fn sqlite3_wal_checkpoint_v2(
            db: *mut sqlite3,
            name: *const c_char,
            mode: c_int,
            log: *mut c_int,
            ckpt: *mut c_int,
        ) -> c_int;
        pub(super) fn sqlite3_wal_hook(
            db: *mut sqlite3,
            callback: Option<
                unsafe extern "C" fn(*mut c_void, *mut sqlite3, *const c_char, c_int) -> c_int,
            >,
            data: *mut c_void,
        ) -> *mut c_void;
        pub(super) fn sqlite3_db_status64(
            db: *mut sqlite3,
            verb: c_int,
            current: *mut i64,
            high: *mut i64,
            reset: c_int,
        ) -> c_int;
        pub(super) fn twelve_readers_open(db: *mut sqlite3) -> c_int;
        pub(super) fn twelve_readers_close();
        pub(super) fn twelve_readers_end();
        pub(super) fn twelve_feed(
            db: *mut sqlite3,
            out: *mut ReaderBuffer,
            scratch: *mut ReaderBuffer,
        ) -> c_int;
        pub(super) fn twelve_post(db: *mut sqlite3, id: i64, out: *mut ReaderBuffer) -> c_int;
        pub(super) fn twelve_reader_reason() -> c_int;
        #[cfg(twelve_fault_inject)]
        pub(super) fn sqlite3_commit_hook(
            db: *mut sqlite3,
            callback: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
            data: *mut c_void,
        ) -> *mut c_void;
        #[cfg(twelve_fault_inject)]
        pub(super) fn sqlite3_set_authorizer(
            db: *mut sqlite3,
            callback: Option<
                unsafe extern "C" fn(
                    *mut c_void,
                    c_int,
                    *const c_char,
                    *const c_char,
                    *const c_char,
                    *const c_char,
                ) -> c_int,
            >,
            data: *mut c_void,
        ) -> c_int;
    }
}

const SQL: [&str; 14] = [
    "SELECT posts.id, posts.body, posts.created_at, users.username, (SELECT COUNT(*) FROM likes WHERE likes.post_id = posts.id) AS like_count FROM posts INNER JOIN users ON users.id = posts.user_id ORDER BY posts.created_at DESC, posts.id DESC LIMIT 20;",
    "SELECT posts.id, posts.body, posts.created_at, users.username, (SELECT COUNT(*) FROM likes WHERE likes.post_id = posts.id) AS like_count FROM posts INNER JOIN users ON users.id = posts.user_id WHERE posts.id = ?1;",
    "SELECT posts.id FROM posts ORDER BY posts.created_at DESC, posts.id DESC LIMIT 20;",
    "SELECT likes.post_id FROM likes WHERE likes.post_id BETWEEN ?1 AND ?2;",
    "SELECT posts.id, posts.body, posts.created_at, users.username FROM posts INNER JOIN users ON users.id = posts.user_id WHERE posts.id BETWEEN ?1 AND ?2;",
    "SELECT posts.id, posts.body, posts.created_at, users.username FROM posts INNER JOIN users ON users.id = posts.user_id WHERE posts.id BETWEEN ?1 AND ?2 ORDER BY posts.id DESC;",
    "SELECT 1;",
    "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at;",
    "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE posts.id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING;",
    "SELECT 1 FROM posts WHERE posts.id = ?1;",
    "BEGIN;",
    "BEGIN IMMEDIATE;",
    "COMMIT;",
    "ROLLBACK;",
];

const SET: [&str; 8] = [
    "PRAGMA journal_mode = WAL;",
    "PRAGMA synchronous = NORMAL;",
    "PRAGMA foreign_keys = ON;",
    "PRAGMA wal_autocheckpoint = 0;",
    "PRAGMA journal_size_limit = 41943040;",
    "PRAGMA busy_timeout = 0;",
    "PRAGMA trusted_schema = OFF;",
    "PRAGMA temp_store = MEMORY;",
];
const GET: [&str; 9] = [
    "PRAGMA foreign_keys;",
    "PRAGMA synchronous;",
    "PRAGMA wal_autocheckpoint;",
    "PRAGMA journal_size_limit;",
    "PRAGMA busy_timeout;",
    "PRAGMA trusted_schema;",
    "PRAGMA temp_store;",
    "PRAGMA cache_size;",
    "PRAGMA mmap_size;",
];

pub(crate) struct StoreSettings {
    pub(crate) check_reads: bool,
    pub(crate) cache_kib: u32,
    pub(crate) mmap_mib: u32,
}

#[derive(Debug, Eq, PartialEq)]
pub(crate) enum Read {
    Ready,
    NotFound,
    Internal,
    Aborted,
    Space { need: usize },
}

#[derive(Debug, Eq, PartialEq)]
pub(crate) enum Write {
    Created,
    Liked { already_liked: bool },
    NotFound,
    Internal,
    Aborted,
    Space { need: usize },
}

pub(crate) struct Commit {
    pub(crate) frames: u32,
    pub(crate) started: Instant,
}

#[derive(Default)]
pub(crate) struct StoreCounts {
    pub(crate) reader_ok: u64,
    pub(crate) reader_not_found: u64,
    pub(crate) reader_space: u64,
    pub(crate) fallback_txn: u64,
    pub(crate) fallback_cookie: u64,
    pub(crate) fallback_cursor: u64,
    pub(crate) fallback_record: u64,
    pub(crate) fallback_span: u64,
    pub(crate) fallback_missing: u64,
    pub(crate) checked: u64,
    pub(crate) commit_fail: u64,
    pub(crate) auto_rollback: u64,
}

#[derive(Clone, Copy, Eq, PartialEq)]
enum Txn {
    Idle,
    Read,
    Write,
    Batch,
    Failed,
    Broken,
}

struct HookState {
    frames: Cell<u32>,
    wrote: Cell<bool>,
}

#[derive(Debug)]
struct Statement {
    ptr: NonNull<sys::sqlite3_stmt>,
}

impl Statement {
    fn prepare(db: NonNull<sys::sqlite3>, sql: &str, persistent: bool) -> Result<Self, c_int> {
        let mut raw = ptr::null_mut();
        let length = c_int::try_from(sql.len()).map_err(|_| 18)?;
        // SAFETY: db is live; sql has exactly length readable bytes and the output
        // pointer is valid. SQLite copies the text and retains no Rust borrow.
        let code = unsafe {
            sqlite3_prepare_v3(
                db.as_ptr(),
                sql.as_ptr().cast(),
                length,
                u32::from(persistent),
                &mut raw,
                ptr::null_mut(),
            )
        };
        if code != OK {
            return Err(code);
        }
        let ptr = NonNull::new(raw).ok_or(ERROR)?;
        Ok(Self { ptr })
    }

    fn finish<T>(&mut self, result: Result<T, c_int>) -> Result<T, c_int> {
        // SAFETY: this owner retains the prepared statement until Drop. Reset and
        // clear end all SQLITE_STATIC borrows before the method returns.
        let reset = unsafe { sqlite3_reset(self.ptr.as_ptr()) };
        // SAFETY: the same live statement remains valid after reset, even on error.
        let clear = unsafe { sqlite3_clear_bindings(self.ptr.as_ptr()) };
        let value = result?;
        check(reset)?;
        check(clear)?;
        Ok(value)
    }

    fn run<T>(&mut self, action: impl FnOnce(&mut Self) -> Result<T, c_int>) -> Result<T, c_int> {
        let result = action(self);
        self.finish(result)
    }

    fn step(&mut self) -> Result<bool, c_int> {
        // SAFETY: the statement is live and only its loop-thread owner steps it.
        match unsafe { sqlite3_step(self.ptr.as_ptr()) } {
            ROW => Ok(true),
            DONE => Ok(false),
            code => Err(code),
        }
    }

    fn done(&mut self) -> Result<(), c_int> {
        if self.step()? { Err(ERROR) } else { Ok(()) }
    }

    fn bind(&mut self, at: c_int, value: i64) -> Result<(), c_int> {
        // SAFETY: SQLite validates at; the scalar binding has no borrowed storage.
        check(unsafe { sqlite3_bind_int64(self.ptr.as_ptr(), at, value) })
    }

    /// Binds `text` at parameter `at` without copying it (SQLITE_STATIC).
    ///
    /// # Safety
    /// `text` must stay alive and unchanged until `run` or `finish` has reset this statement and
    /// cleared its bindings, since SQLite reads the bound pointer until then.
    unsafe fn text_binding(&mut self, at: c_int, text: &str) -> Result<(), c_int> {
        let length = c_int::try_from(text.len()).map_err(|_| 18)?;
        // SAFETY: this function's contract keeps text alive until run/finish has reset and
        // cleared bindings on every path. The positive explicit count preserves NUL bytes.
        check(unsafe {
            sqlite3_bind_text(self.ptr.as_ptr(), at, text.as_ptr().cast(), length, None)
        })
    }

    /// The integer value of column `at` of the current row.
    ///
    /// # Safety
    /// The statement's last step must have returned a row, with no step or reset since, and `at`
    /// must be one of that row's column indices; SQLite leaves any other call undefined.
    unsafe fn integer(&self, at: c_int) -> i64 {
        // SAFETY: this function's contract gives a current row and a column index inside it.
        unsafe { sqlite3_column_int64(self.ptr.as_ptr(), at) }
    }

    /// The datatype code of column `at` of the current row.
    ///
    /// # Safety
    /// The statement's last step must have returned a row, with no step or reset since, and `at`
    /// must be one of that row's column indices; SQLite leaves any other call undefined.
    unsafe fn column_type(&self, at: c_int) -> c_int {
        // SAFETY: this function's contract gives a current row and a column index inside it.
        unsafe { sqlite3_column_type(self.ptr.as_ptr(), at) }
    }

    /// The text bytes of column `at` of the current row, borrowed from SQLite until the next step
    /// or reset, which the borrow of self rules out meanwhile.
    ///
    /// # Safety
    /// The statement's last step must have returned a row, with no step or reset since, and `at`
    /// must be one of that row's column indices; SQLite leaves any other call undefined.
    unsafe fn text(&self, at: c_int) -> Result<&[u8], c_int> {
        // SAFETY: this function's contract gives a current row and a column index inside it.
        // Conversion precedes the byte count as required by SQLite; no other column conversion
        // occurs before the slice is used.
        let bytes = unsafe { sqlite3_column_text(self.ptr.as_ptr(), at) };
        // SAFETY: the same row/column is live and its text conversion is complete.
        let length = unsafe { sqlite3_column_bytes(self.ptr.as_ptr(), at) };
        let length = usize::try_from(length).map_err(|_| ERROR)?;
        if length == 0 {
            return Ok(&[]);
        }
        if bytes.is_null() {
            return Err(NOMEM);
        }
        // SAFETY: SQLite owns length readable bytes until the next step/reset.
        // The slice borrows self, so callers cannot step while it is in use.
        Ok(unsafe { std::slice::from_raw_parts(bytes, length) })
    }
}

impl Drop for Statement {
    fn drop(&mut self) {
        // SAFETY: this is the sole owner of a handle from a successful prepare, finalized only
        // here; no binding or row borrow outlives it.
        unsafe { sqlite3_finalize(self.ptr.as_ptr()) };
    }
}

fn check(code: c_int) -> Result<(), c_int> {
    if code == OK { Ok(()) } else { Err(code) }
}

fn open_error(stage: &str, code: c_int) -> String {
    format!("store open: {stage} (sqlite {})", code & 255)
}

// The open stage names the path it failed on.
fn open_path_error(path: &str, code: c_int) -> String {
    format!(
        "store open: open {} (sqlite {})",
        path.escape_debug(),
        code & 255
    )
}

fn invariant(kind: &str) -> ! {
    // One write, whole or not at all (stderr_line); the process ends with status 1 either way.
    let _ = stderr_line(|out| writeln!(out, "store invariant: {kind}"));
    std::process::exit(1)
}

fn escape_len(text: &[u8]) -> Option<usize> {
    text.iter().try_fold(2_usize, |length, byte| {
        length.checked_add(match byte {
            b'"' | b'\\' | b'\n' | b'\r' | b'\t' => 2,
            0..=31 => 6,
            _ => 1,
        })
    })
}

struct BodyWriter<'a> {
    out: &'a mut Vec<u8>,
    base: usize,
    need: Option<usize>,
}

impl<'a> BodyWriter<'a> {
    fn new(out: &'a mut Vec<u8>) -> Self {
        Self {
            base: out.len(),
            out,
            need: Some(0),
        }
    }

    fn admit(&mut self, length: Option<usize>) -> bool {
        let previous = self.need;
        self.need = previous.zip(length).and_then(|(a, b)| a.checked_add(b));
        self.need.is_some_and(|need| {
            need <= BODY_MAX
                && need <= self.out.capacity() - self.base
                && previous == Some(self.out.len() - self.base)
        })
    }

    fn put(&mut self, bytes: &[u8]) {
        if self.admit(Some(bytes.len())) {
            self.out.extend_from_slice(bytes);
        }
    }

    fn string(&mut self, bytes: &[u8]) {
        if self.admit(escape_len(bytes)) {
            crate::json::escape_into(self.out, bytes);
        }
    }

    fn number(&mut self, mut value: u64) {
        let mut bytes = [0_u8; 20];
        let mut at = bytes.len();
        loop {
            at -= 1;
            *bytes
                .get_mut(at)
                .expect("a u64 has at most 20 decimal digits") =
                b'0' + u8::try_from(value % 10).expect("a decimal digit fits in u8");
            value /= 10;
            if value == 0 {
                break;
            }
        }
        self.put(
            bytes
                .get(at..)
                .expect("a u64 has at most 20 decimal digits"),
        );
    }

    /// Renders the post object of the statement's current row.
    ///
    /// # Safety
    /// The statement's last step must have returned a row, with no step or reset since, of one of
    /// the four post queries (catalog statements 0, 1, 4 and 5), whose columns 0 to 3 are the post
    /// id, body, created_at and username.
    unsafe fn object(&mut self, statement: &Statement, likes: u64) -> Result<(), c_int> {
        self.put(b"{\"id\":");
        // SQLite rowids use the reader's deliberate two-complement representation.
        // SAFETY: this function's contract gives a current row with column 0, the post id.
        self.number(unsafe { statement.integer(0) }.cast_unsigned());
        self.put(b",\"body\":");
        // SAFETY: this function's contract gives a current row with column 1, the body.
        self.string(unsafe { statement.text(1) }?);
        self.put(b",\"created_at\":");
        // SAFETY: this function's contract gives a current row with column 2, created_at.
        self.string(unsafe { statement.text(2) }?);
        self.put(b",\"author\":");
        // SAFETY: this function's contract gives a current row with column 3, the username.
        self.string(unsafe { statement.text(3) }?);
        self.put(b",\"like_count\":");
        self.number(likes);
        self.put(b"}");
        Ok(())
    }

    fn finish(self) -> Read {
        match self.need {
            Some(need) if need <= BODY_MAX && self.out.len() - self.base == need => Read::Ready,
            Some(need) if need <= BODY_MAX => {
                self.out.truncate(self.base);
                Read::Space { need }
            }
            _ => {
                self.out.truncate(self.base);
                Read::Internal
            }
        }
    }
}

pub(crate) struct Store {
    db: NonNull<sys::sqlite3>,
    // Drop finalizes these before it closes db, so they are dropped by hand, not as a field.
    statements: ManuallyDrop<[Statement; 14]>,
    hook: Box<HookState>,
    check_reads: bool,
    readers_opened: bool,
    txn: Txn,
    scratch: Vec<u8>,
    comparison: Vec<u8>,
    pending_commit: Option<Commit>,
    // The loop's clock reading for this turn (tick), taken once after its wait; a commit's start time.
    now: Instant,
    counts: StoreCounts,
    #[cfg(twelve_fault_inject)]
    fault: fault::FaultVfs,
    #[cfg(twelve_fault_inject)]
    successful_writes: usize,
    #[cfg(twelve_fault_inject)]
    auto_injected: bool,
}

impl Store {
    pub(crate) fn open(path: &str, settings: &StoreSettings) -> Result<Self, String> {
        if path.is_empty() {
            return Err(open_error("path", 0));
        }
        let path_c = CString::new(path).map_err(|_| open_error("path", 0))?;
        if !(1000..=262_144).contains(&settings.cache_kib) || settings.mmap_mib > 1024 {
            return Err(open_error("settings", 0));
        }
        let mmap = i64::from(settings.mmap_mib)
            .checked_mul(1_048_576)
            .ok_or_else(|| open_error("settings", 0))?;
        #[cfg(twelve_fault_inject)]
        let fault = fault::FaultVfs::new(path).map_err(|code| open_error("vfs", code))?;
        #[cfg(twelve_fault_inject)]
        let vfs = c"twelve-store-fault";
        #[cfg(not(twelve_fault_inject))]
        let vfs = c"unix-excl";
        let mut db = ptr::null_mut();
        // SAFETY: the path/vfs C strings remain live through open; db is writable.
        // No CREATE or URI flag is used and all calls remain on this loop thread.
        let code = unsafe { sqlite3_open_v2(path_c.as_ptr(), &mut db, 0x8002, vfs.as_ptr()) };
        let Some(db) = NonNull::new(db) else {
            return Err(open_path_error(path, code));
        };
        let statements = match Self::configure(db, code, path, settings, mmap) {
            Ok(statements) => statements,
            Err(error) => {
                // SAFETY: configure finalized every statement it prepared and registered no
                // callback, so nothing else uses db; it is closed exactly once, here.
                unsafe { sqlite3_close(db.as_ptr()) };
                return Err(error);
            }
        };
        let mut store = Self {
            db,
            statements: ManuallyDrop::new(statements),
            hook: Box::new(HookState {
                frames: Cell::new(0),
                wrote: Cell::new(false),
            }),
            check_reads: settings.check_reads,
            readers_opened: false,
            txn: Txn::Idle,
            scratch: Vec::new(),
            comparison: Vec::new(),
            pending_commit: None,
            now: Instant::now(),
            counts: StoreCounts::default(),
            #[cfg(twelve_fault_inject)]
            fault,
            #[cfg(twelve_fault_inject)]
            successful_writes: 0,
            #[cfg(twelve_fault_inject)]
            auto_injected: false,
        };
        store
            .scratch
            .try_reserve_exact(BODY_MAX)
            .map_err(|_| open_error("prepare", NOMEM))?;
        if settings.check_reads {
            store
                .comparison
                .try_reserve_exact(BODY_MAX)
                .map_err(|_| open_error("prepare", NOMEM))?;
        }
        // SAFETY: persistent statement preparation loaded the schema; this Store
        // is the one process reader owner and retains db until reader close.
        let opened = unsafe { twelve_readers_open(db.as_ptr()) };
        match ReaderOpen::try_from(opened) {
            Ok(ReaderOpen::Opened) => store.readers_opened = true,
            Ok(ReaderOpen::Refused) => {}
            Err(_) => invariant("reader-result"),
        }
        let (mut log, mut ckpt) = (-1, -1);
        // SAFETY: output scalars are writable; NOOP observes without a pass/sync.
        let code = unsafe {
            sqlite3_wal_checkpoint_v2(db.as_ptr(), c"main".as_ptr(), -1, &mut log, &mut ckpt)
        };
        check(code).map_err(|code| open_error("wal-status", code))?;
        let frames = if log == -1 && ckpt == -1 {
            0
        } else {
            if log < 0 || ckpt < 0 || ckpt > log {
                return Err(open_error("wal-status", ERROR));
            }
            u32::try_from(log).map_err(|_| open_error("wal-status", ERROR))?
        };
        store.hook.frames.set(frames);
        // SAFETY: the box pins callback state through callback removal in Drop.
        // The callback mutates only Cells and never calls back into SQLite.
        unsafe {
            sqlite3_wal_hook(
                db.as_ptr(),
                Some(wal_callback),
                (&*store.hook as *const HookState).cast_mut().cast(),
            )
        };
        for verb in [7, 8] {
            store
                .cache_value(verb)
                .map_err(|code| open_error("cache-status", code))?;
        }
        #[cfg(twelve_fault_inject)]
        // SAFETY: the fault box is pinned and outlives callback unregistration.
        unsafe {
            sqlite3_commit_hook(
                db.as_ptr(),
                Some(fault::commit_callback),
                store.fault.data(),
            )
        };
        Ok(store)
    }

    // The open steps that run before Store exists: the read-write check, the settings and their
    // readback, then the fourteen persistent statements, which become Store's array only once all
    // are prepared, so a failed prepare finalizes exactly those prepared before it.
    fn configure(
        db: NonNull<sys::sqlite3>,
        code: c_int,
        path: &str,
        settings: &StoreSettings,
        mmap: i64,
    ) -> Result<[Statement; 14], String> {
        check(code).map_err(|code| open_path_error(path, code))?;
        // SAFETY: the successful open supplies a live main-database connection.
        if unsafe { sqlite3_db_readonly(db.as_ptr(), c"main".as_ptr()) } != 0 {
            return Err(open_path_error(path, 8));
        }
        for (index, sql) in SET.iter().enumerate() {
            let mut statement =
                Statement::prepare(db, sql, false).map_err(|code| open_error("pragma", code))?;
            statement
                .run(|s| {
                    if index == 0 {
                        // SAFETY: || reads column 0 only after step returned the pragma's one
                        // row, its one column the journal mode.
                        if !s.step()? || unsafe { s.text(0) }? != b"wal" {
                            return Err(ERROR);
                        }
                        s.done()
                    } else {
                        while s.step()? {}
                        Ok(())
                    }
                })
                .map_err(|code| open_error("pragma", code))?;
        }
        for sql in [
            format!("PRAGMA cache_size = -{};", settings.cache_kib),
            format!("PRAGMA mmap_size = {mmap};"),
        ] {
            let mut statement =
                Statement::prepare(db, &sql, false).map_err(|code| open_error("pragma", code))?;
            statement
                .run(|s| {
                    while s.step()? {}
                    Ok(())
                })
                .map_err(|code| open_error("pragma", code))?;
        }
        // One expected value per GET pragma, in GET's order; the length is GET's, so zip drops none.
        let expected: [i64; GET.len()] = [
            1,
            1,
            0,
            41_943_040,
            0,
            0,
            2,
            -i64::from(settings.cache_kib),
            mmap,
        ];
        for (sql, expected) in GET.iter().zip(expected) {
            let mut statement =
                Statement::prepare(db, sql, false).map_err(|code| open_error("readback", code))?;
            statement
                .run(|s| {
                    if !s.step()?
                        // SAFETY: || reads column 0 only after step returned the pragma's one
                        // row, its one column the setting's value.
                        || unsafe { s.column_type(0) } != 1
                        // SAFETY: the same row and column, read only after the line above.
                        || unsafe { s.integer(0) } != expected
                    {
                        return Err(ERROR);
                    }
                    s.done()
                })
                .map_err(|code| open_error("readback", code))?;
        }
        let mut prepared = Vec::with_capacity(SQL.len());
        for sql in SQL {
            prepared.push(
                Statement::prepare(db, sql, true).map_err(|code| open_error("prepare", code))?,
            );
        }
        Ok(<[Statement; 14]>::try_from(prepared).expect("the catalog holds fourteen statements"))
    }

    fn statement(&mut self, index: usize) -> &mut Statement {
        self.statements
            .get_mut(index)
            .expect("each catalog index names one of the fourteen statements")
    }

    fn control(&mut self, index: usize) -> Result<(), c_int> {
        self.statement(index).run(Statement::done)
    }

    fn autocommit(&self) -> bool {
        // SAFETY: db belongs to this Store and remains live until Drop.
        unsafe { sqlite3_get_autocommit(self.db.as_ptr()) != 0 }
    }

    fn begin(&mut self, state: Txn, index: usize) -> bool {
        if self.txn != Txn::Idle {
            invariant("txn-state");
        }
        self.pending_commit = None;
        self.hook.wrote.set(false);
        #[cfg(twelve_fault_inject)]
        {
            self.successful_writes = 0;
            self.auto_injected = false;
        }
        if self.control(index).is_ok() && !self.autocommit() {
            self.txn = state;
            true
        } else {
            self.txn = Txn::Failed;
            self.rollback();
            false
        }
    }

    pub(crate) fn begin_read(&mut self) -> bool {
        self.begin(Txn::Read, 10)
    }
    pub(crate) fn begin_write(&mut self) -> bool {
        self.begin(Txn::Write, 11)
    }
    pub(crate) fn begin_batch(&mut self) -> bool {
        self.begin(Txn::Batch, 10)
    }

    pub(crate) fn txn_alive(&mut self) -> bool {
        if matches!(self.txn, Txn::Read | Txn::Write | Txn::Batch) {
            if !self.autocommit() {
                return true;
            }
            self.txn = Txn::Failed;
            self.counts.auto_rollback = self.counts.auto_rollback.saturating_add(1);
        }
        false
    }

    /// Records the loop's clock reading for this turn, which the turn's commits take as their start
    /// time.
    pub(crate) fn tick(&mut self, now: Instant) {
        self.now = now;
    }

    /// Closes the readers' cursors, which stay open across the reader calls of one transaction; runs
    /// before the transaction writes, commits or rolls back.
    fn end_readers(&mut self) {
        if self.readers_opened {
            // SAFETY: the readers' connection is open, and twelve_readers_end only closes the cursors
            // the readers opened inside its current transaction.
            unsafe { twelve_readers_end() };
        }
    }

    pub(crate) fn end_read(&mut self) -> bool {
        self.commit()
    }

    pub(crate) fn commit(&mut self) -> bool {
        self.end_readers();
        if !self.txn_alive() {
            self.pending_commit = None;
            if self.txn == Txn::Failed {
                self.rollback();
            }
            return false;
        }
        #[cfg(debug_assertions)]
        for statement in self.statements.iter() {
            // SAFETY: successful open prepared each owned handle; no statement
            // can be finalized or borrowed by another thread before this check.
            let busy = unsafe { sqlite3_stmt_busy(statement.ptr.as_ptr()) };
            debug_assert_eq!(busy, 0);
        }
        self.hook.wrote.set(false);
        let started = self.now;
        let result = self.control(12);
        if result.is_ok() && self.autocommit() {
            self.txn = Txn::Idle;
            if self.hook.wrote.get() {
                self.pending_commit = Some(Commit {
                    frames: self.hook.frames.get(),
                    started,
                });
            }
            true
        } else {
            self.counts.commit_fail = self.counts.commit_fail.saturating_add(1);
            self.txn = Txn::Failed;
            self.pending_commit = None;
            self.rollback();
            false
        }
    }

    pub(crate) fn rollback(&mut self) -> bool {
        self.end_readers();
        if self.txn == Txn::Broken {
            return false;
        }
        if self.txn == Txn::Idle {
            return true;
        }
        if !self.autocommit() {
            let _ = self.control(13);
        }
        self.pending_commit = None;
        if self.autocommit() {
            self.txn = Txn::Idle;
            true
        } else {
            self.txn = Txn::Broken;
            false
        }
    }

    fn read_error(&mut self, code: c_int) -> Read {
        let alive = self.txn_alive();
        if matches!(code & 255, 7 | 9 | 10 | 13) {
            self.txn = Txn::Failed;
        }
        if !alive || self.txn == Txn::Failed {
            Read::Aborted
        } else {
            Read::Internal
        }
    }

    fn write_error(&mut self, code: c_int) -> Write {
        if self.txn_alive() && code & 255 == CONSTRAINT {
            Write::Internal
        } else {
            self.txn = Txn::Failed;
            Write::Aborted
        }
    }

    pub(crate) fn feed(&mut self, out: &mut Vec<u8>) -> Read {
        self.read(None, out)
    }
    pub(crate) fn post(&mut self, id: i64, out: &mut Vec<u8>) -> Read {
        self.read(Some(id), out)
    }

    fn read(&mut self, id: Option<i64>, out: &mut Vec<u8>) -> Read {
        if !self.txn_alive() {
            return Read::Aborted;
        }
        let base = out.len();
        if !self.readers_opened {
            return self.sql_read(id, out);
        }
        self.scratch.clear();
        self.comparison.clear();
        let capacity = if self.check_reads {
            BODY_MAX
        } else {
            (out.capacity() - base).min(BODY_MAX)
        };
        let bytes = if self.check_reads {
            self.comparison.as_mut_ptr()
        } else {
            // SAFETY: base is the current Vec length, within its allocation.
            unsafe { out.as_mut_ptr().add(base) }
        };
        let mut target = ReaderBuffer {
            ptr: bytes,
            cap: capacity,
            len: 0,
            need: 0,
        };
        let mut scratch = ReaderBuffer {
            ptr: self.scratch.as_mut_ptr(),
            cap: BODY_MAX,
            len: 0,
            need: 0,
        };
        let result = match id {
            // SAFETY: target's extent is allocated, writable and live for this synchronous call;
            // no length is published until the result is validated.
            Some(id) => unsafe { twelve_post(self.db.as_ptr(), id, &mut target) },
            // SAFETY: target's and scratch's extents are distinct, allocated, writable and live for
            // this synchronous call; no length is published until the result is validated.
            None => unsafe { twelve_feed(self.db.as_ptr(), &mut target, &mut scratch) },
        };
        // SAFETY: reason is read immediately after the matching reader invocation.
        let reason = unsafe { twelve_reader_reason() };
        let Ok(status) = ReaderStatus::try_from(result) else {
            invariant("reader-result");
        };
        let Ok(reason) = ReaderReason::try_from(reason) else {
            invariant("reader-reason");
        };
        if (status == ReaderStatus::Fallback) == (reason == ReaderReason::None) {
            invariant("reader-reason");
        }
        if target.len > target.cap || scratch.len > scratch.cap {
            invariant("reader-buffer");
        }
        let reason_name = match status {
            ReaderStatus::Ok => {
                self.counts.reader_ok = self.counts.reader_ok.saturating_add(1);
                None
            }
            ReaderStatus::NotFound => {
                self.counts.reader_not_found = self.counts.reader_not_found.saturating_add(1);
                None
            }
            ReaderStatus::Space => {
                self.counts.reader_space = self.counts.reader_space.saturating_add(1);
                Some("space")
            }
            ReaderStatus::Fallback => {
                let (counter, name) = match reason {
                    ReaderReason::Txn => (&mut self.counts.fallback_txn, "txn"),
                    ReaderReason::Cookie => (&mut self.counts.fallback_cookie, "cookie"),
                    ReaderReason::Cursor => (&mut self.counts.fallback_cursor, "cursor"),
                    ReaderReason::Record => (&mut self.counts.fallback_record, "record"),
                    ReaderReason::Span => (&mut self.counts.fallback_span, "span"),
                    ReaderReason::Missing => (&mut self.counts.fallback_missing, "missing"),
                    ReaderReason::None => invariant("reader-reason"),
                };
                *counter = counter.saturating_add(1);
                Some(name)
            }
        };
        if status == ReaderStatus::Ok {
            if target.need != 0 || scratch.need != 0 {
                invariant("reader-buffer");
            }
            if self.check_reads {
                // SAFETY: OK initialized exactly target.len bytes, at most target.cap (checked
                // above), of comparison's reserved extent, which never aliases scratch.
                unsafe { self.comparison.set_len(target.len) };
            } else {
                // SAFETY: OK initialized exactly target.len bytes, at most target.cap (checked
                // above), of out's spare capacity after base, which never aliases scratch.
                unsafe { out.set_len(base + target.len) };
            }
        }
        let answered = matches!(status, ReaderStatus::Ok | ReaderStatus::NotFound);
        if !self.check_reads && answered {
            return if status == ReaderStatus::Ok {
                Read::Ready
            } else {
                Read::NotFound
            };
        }
        if self.check_reads
            && let Some(reason) = reason_name
        {
            // One write, whole or not at all (stderr_line); a dropped or failed line leaves the
            // fallback answer unchanged.
            let _ = stderr_line(|stderr| match id {
                Some(id) => writeln!(stderr, "reader fallback: /posts/{id} reason={reason}"),
                None => writeln!(stderr, "reader fallback: /feed reason={reason}"),
            });
        }
        let sql = self.sql_read(id, out);
        if self.check_reads
            && answered
            && !matches!(sql, Read::Space { .. } | Read::Internal | Read::Aborted)
        {
            self.counts.checked = self.counts.checked.saturating_add(1);
            let body = out
                .get(base..)
                .expect("sql_read leaves out at or past its starting length");
            if (status == ReaderStatus::Ok && (sql != Read::Ready || self.comparison != body))
                || (status == ReaderStatus::NotFound && sql != Read::NotFound)
            {
                // One write, whole or not at all (stderr_line); the mismatch exits with status 70
                // whether or not this line is written.
                let _ = stderr_line(|stderr| match id {
                    Some(id) => writeln!(stderr, "reader mismatch: /posts/{id}"),
                    None => writeln!(stderr, "reader mismatch: /feed"),
                });
                std::process::exit(70);
            }
        }
        self.scratch.clear();
        self.comparison.clear();
        sql
    }

    fn sql_read(&mut self, id: Option<i64>, out: &mut Vec<u8>) -> Read {
        let base = out.len();
        let result = if let Some(id) = id {
            self.sql_post(id, out)
        } else {
            self.sql_feed(out)
        };
        self.scratch.clear();
        match result {
            Ok(result) => result,
            Err(code) => {
                out.truncate(base);
                self.read_error(code)
            }
        }
    }

    fn sql_post(&mut self, id: i64, out: &mut Vec<u8>) -> Result<Read, c_int> {
        let mut writer = BodyWriter::new(out);
        let found = self.statement(1).run(|s| {
            s.bind(1, id)?;
            if !s.step()? {
                return Ok(false);
            }
            writer.put(b"{\"post\":");
            // SAFETY: step returned a row of statement 1, the post query, whose column 4 is
            // like_count.
            let likes = unsafe { s.integer(4) };
            // A COUNT(*) is never negative; cast_unsigned states the readers' two's-complement
            // u64 reading of every SQLite integer.
            // SAFETY: step returned a row of statement 1, one of the four post queries object
            // reads.
            unsafe { writer.object(s, likes.cast_unsigned()) }?;
            writer.put(b"}");
            s.done()?;
            Ok(true)
        })?;
        Ok(if found {
            writer.finish()
        } else {
            Read::NotFound
        })
    }

    fn reference_feed(&mut self, out: &mut Vec<u8>) -> Result<Read, c_int> {
        let mut writer = BodyWriter::new(out);
        writer.put(b"{\"posts\":[");
        self.statement(0).run(|s| {
            let mut first = true;
            while s.step()? {
                if !first {
                    writer.put(b",");
                }
                first = false;
                // SAFETY: step returned a row of statement 0, the reference feed query, whose
                // column 4 is like_count.
                let likes = unsafe { s.integer(4) };
                // A COUNT(*) is never negative; cast_unsigned states the readers' two's-complement
                // u64 reading of every SQLite integer.
                // SAFETY: step returned a row of statement 0, one of the four post queries object
                // reads.
                unsafe { writer.object(s, likes.cast_unsigned()) }?;
            }
            Ok(())
        })?;
        writer.put(b"]}");
        Ok(writer.finish())
    }

    fn sql_feed(&mut self, out: &mut Vec<u8>) -> Result<Read, c_int> {
        let base = out.len();
        let mut ids = [0_i64; 20];
        let count = self.statement(2).run(|s| {
            let mut count = 0;
            while s.step()? {
                if count == ids.len() {
                    return Err(ERROR);
                }
                // SAFETY: step returned a row of statement 2, the feed id query, whose column 0 is
                // the post id.
                let id = unsafe { s.integer(0) };
                *ids.get_mut(count)
                    .expect("count is below twenty, checked just above") = id;
                count += 1;
            }
            Ok(count)
        })?;
        if count == 0 {
            let mut writer = BodyWriter::new(out);
            writer.put(b"{\"posts\":[]}");
            return Ok(writer.finish());
        }
        let selected = ids
            .get(..count)
            .expect("the discovery list holds at most twenty ids");
        let min = *selected
            .iter()
            .min()
            .expect("the discovery list is nonempty");
        let max = *selected
            .iter()
            .max()
            .expect("the discovery list is nonempty");
        if max.checked_sub(min).is_none_or(|span| span >= 256) {
            return self.reference_feed(out);
        }
        let mut slots = [-1_i8; 256];
        for (slot, &id) in selected.iter().enumerate() {
            let at = usize::try_from(id - min).expect("the checked span is below 256");
            let entry = slots.get_mut(at).expect("the checked span is below 256");
            if *entry >= 0 {
                return self.reference_feed(out);
            }
            *entry = i8::try_from(slot).expect("the feed has at most twenty slots");
        }
        let mut likes = [0_u64; 20];
        self.statement(3).run(|s| {
            s.bind(1, min)?;
            s.bind(2, max)?;
            while s.step()? {
                // SAFETY: step returned a row of statement 3, the like range query, whose column 0
                // is the post id.
                if let Some(slot) = selected_slot(unsafe { s.integer(0) }, min, &slots) {
                    let like = likes
                        .get_mut(slot)
                        .expect("a selected slot is below twenty");
                    *like = like.checked_add(1).ok_or(ERROR)?;
                }
            }
            Ok(())
        })?;
        let first = *selected.first().expect("the discovery list is nonempty");
        let contiguous = selected.iter().enumerate().all(|(i, id)| {
            first.checked_sub(i64::try_from(i).expect("at most twenty indices")) == Some(*id)
        });
        if contiguous {
            let mut writer = BodyWriter::new(out);
            writer.put(b"{\"posts\":[");
            let matched = self.statement(5).run(|s| {
                s.bind(1, min)?;
                s.bind(2, max)?;
                let mut row = 0;
                let mut matched = true;
                while s.step()? {
                    // SAFETY: step returned a row of statement 5, the descending row range query,
                    // whose column 0 is the post id.
                    if selected.get(row) != Some(&unsafe { s.integer(0) }) {
                        matched = false;
                    } else {
                        if row != 0 {
                            writer.put(b",");
                        }
                        // SAFETY: step returned a row of statement 5, one of the four post queries
                        // object reads.
                        unsafe {
                            writer.object(
                                s,
                                *likes
                                    .get(row)
                                    .expect("a matched row is below count, at most twenty"),
                            )
                        }?;
                    }
                    row += 1;
                }
                Ok(matched && row == count)
            })?;
            if matched {
                writer.put(b"]}");
                return Ok(writer.finish());
            }
        } else {
            self.scratch.clear();
            let mut fragments = [(0_usize, 0_usize); 20];
            let mut found = [false; 20];
            let matched = self
                .statements
                .get_mut(4)
                .expect("the catalog holds fourteen statements")
                .run(|s| {
                    s.bind(1, min)?;
                    s.bind(2, max)?;
                    let mut matched = true;
                    while s.step()? {
                        // SAFETY: step returned a row of statement 4, the row range query, whose
                        // column 0 is the post id.
                        if let Some(slot) = selected_slot(unsafe { s.integer(0) }, min, &slots) {
                            let seen = found
                                .get_mut(slot)
                                .expect("a selected slot is below twenty");
                            if *seen {
                                matched = false;
                                continue;
                            }
                            *seen = true;
                            let start = self.scratch.len();
                            let mut writer = BodyWriter::new(&mut self.scratch);
                            // SAFETY: step returned a row of statement 4, one of the four post
                            // queries object reads.
                            unsafe {
                                writer.object(
                                    s,
                                    *likes.get(slot).expect("a selected slot is below twenty"),
                                )
                            }?;
                            if writer.finish() != Read::Ready {
                                matched = false;
                            }
                            *fragments
                                .get_mut(slot)
                                .expect("a selected slot is below twenty") =
                                (start, self.scratch.len());
                        }
                    }
                    Ok(matched
                        && found
                            .get(..count)
                            .expect("count is at most twenty")
                            .iter()
                            .all(|found| *found))
                })?;
            if matched {
                let mut writer = BodyWriter::new(out);
                writer.put(b"{\"posts\":[");
                for (index, &(start, end)) in fragments
                    .get(..count)
                    .expect("count is at most twenty")
                    .iter()
                    .enumerate()
                {
                    if index != 0 {
                        writer.put(b",");
                    }
                    writer.put(
                        self.scratch
                            .get(start..end)
                            .expect("each fragment lies inside scratch"),
                    );
                }
                writer.put(b"]}");
                return Ok(writer.finish());
            }
        }
        out.truncate(base);
        self.scratch.clear();
        self.reference_feed(out)
    }

    pub(crate) fn health(&mut self) -> Result<(), String> {
        if self.txn != Txn::Idle && !self.txn_alive() {
            return Err("transaction aborted".into());
        }
        #[cfg(twelve_fault_inject)]
        let deny = self.fault.health_failed();
        #[cfg(twelve_fault_inject)]
        if deny {
            // SAFETY: the callback uses no captured data and only denies SELECT;
            // removal below runs on every path before this method returns.
            let code = unsafe {
                sqlite3_set_authorizer(
                    self.db.as_ptr(),
                    Some(fault::health_authorizer),
                    ptr::null_mut(),
                )
            };
            if code != OK {
                let message = self.message();
                // SAFETY: db is still live; unwind the temporary callback even
                // when installation failed, retaining its first diagnostic.
                unsafe { sqlite3_set_authorizer(self.db.as_ptr(), None, ptr::null_mut()) };
                return Err(message);
            }
        }
        let statement = self
            .statements
            .get_mut(6)
            .expect("the catalog holds fourteen statements");
        let result = (|| {
            if !statement.step()?
                // SAFETY: || reads column 0 only after step returned a row of statement 6,
                // SELECT 1, whose one column is the selected 1.
                || unsafe { statement.column_type(0) } != 1
                // SAFETY: the same row and column, read only after the line above.
                || unsafe { statement.integer(0) } != 1
            {
                return Ok(false);
            }
            if statement.step()? {
                return Ok(false);
            }
            Ok(true)
        })();
        let message = result.as_ref().err().map(|_| database_message(self.db));
        let result = statement.finish(result);
        #[cfg(twelve_fault_inject)]
        if deny {
            // SAFETY: removing the authorizer ends the temporary fault policy.
            let code = unsafe { sqlite3_set_authorizer(self.db.as_ptr(), None, ptr::null_mut()) };
            if code != OK && result.is_ok() {
                return Err(self.message());
            }
        }
        match result {
            Ok(true) => Ok(()),
            Ok(false) => Err("health query failed".into()),
            Err(code) => {
                if self.txn != Txn::Idle {
                    self.read_error(code);
                }
                Err(message.unwrap_or_else(|| self.message()))
            }
        }
    }

    fn message(&self) -> String {
        database_message(self.db)
    }

    pub(crate) fn create_need(username: &str, body: &str) -> Option<usize> {
        let literal =
            b"{\"post\":{\"id\":,\"body\":,\"created_at\":,\"author\":,\"like_count\":0}}".len();
        literal
            .checked_add(20)?
            .checked_add(26)?
            .checked_add(escape_len(body.as_bytes())?)?
            .checked_add(escape_len(username.as_bytes())?)
            .filter(|need| *need <= BODY_MAX)
    }

    pub(crate) fn like_need(post_id: i64) -> usize {
        let mut digits = 1;
        // cast_unsigned states the readers' deliberate two's-complement u64 reading of a rowid.
        let mut id = post_id.cast_unsigned();
        while id >= 10 {
            id /= 10;
            digits += 1;
        }
        b"{\"liked\":true,\"already_liked\":false,\"post_id\":}".len() + digits
    }

    fn before_write(&mut self) -> bool {
        self.end_readers();
        if !matches!(self.txn, Txn::Write | Txn::Batch) || !self.txn_alive() {
            return false;
        }
        #[cfg(twelve_fault_inject)]
        if self.successful_writes == 1 && !self.auto_injected && self.fault.auto_rollback() {
            self.auto_injected = true;
            let result = Statement::prepare(
                self.db,
                "INSERT OR ROLLBACK INTO likes (user_id, post_id) VALUES (NULL, NULL);",
                false,
            )
            .and_then(|mut s| s.run(Statement::done));
            if !self.txn_alive() {
                return false;
            }
            if let Err(code) = result {
                self.write_error(code);
                return false;
            }
        }
        true
    }

    pub(crate) fn create(
        &mut self,
        user_id: i64,
        username: &str,
        body: &str,
        out: &mut Vec<u8>,
    ) -> Write {
        if !matches!(self.txn, Txn::Write | Txn::Batch) || !self.txn_alive() {
            return Write::Aborted;
        }
        let Some(need) = Self::create_need(username, body) else {
            return Write::Internal;
        };
        if out.capacity() - out.len() < need {
            return Write::Space { need };
        }
        if !self.before_write() {
            return Write::Aborted;
        }
        let base = out.len();
        let mut writer = BodyWriter::new(out);
        let mut inserted = false;
        // The insert runs at the statement's first step, which returns the new row's id and
        // created_at from the same statement.
        let result = self.statement(7).run(|s| {
            s.bind(1, user_id)?;
            // SAFETY: body is create's argument, alive for this whole call, and run's finish
            // resets the statement and clears its bindings before run returns.
            unsafe { s.text_binding(2, body) }?;
            if !s.step()? {
                return Err(ERROR);
            }
            inserted = true;
            // SAFETY: step returned the row of statement 7, the insert's RETURNING id, created_at,
            // whose column 0 is the new post's id.
            let id = unsafe { s.integer(0) };
            // SAFETY: the same row; column 1 is created_at.
            let timestamp = unsafe { s.text(1) }?;
            if !valid_timestamp(timestamp) {
                return Err(ERROR);
            }
            writer.put(b"{\"post\":{\"id\":");
            // cast_unsigned states the readers' deliberate two's-complement u64 reading of a rowid.
            writer.number(id.cast_unsigned());
            writer.put(b",\"body\":");
            writer.string(body.as_bytes());
            writer.put(b",\"created_at\":");
            writer.string(timestamp);
            writer.put(b",\"author\":");
            writer.string(username.as_bytes());
            writer.put(b",\"like_count\":0}}");
            s.done()
        });
        let rendered = match result {
            Ok(()) => writer.finish() == Read::Ready,
            // A failure after the insert, like a missing or malformed created_at, fails the unit.
            Err(_) if inserted => false,
            Err(code) => return self.write_error(code),
        };
        if !rendered || !self.txn_alive() {
            out.truncate(base);
            self.txn = Txn::Failed;
            return Write::Aborted;
        }
        #[cfg(twelve_fault_inject)]
        {
            self.successful_writes = self.successful_writes.saturating_add(1);
        }
        Write::Created
    }

    pub(crate) fn like(&mut self, user_id: i64, post_id: i64, out: &mut Vec<u8>) -> Write {
        if !matches!(self.txn, Txn::Write | Txn::Batch) || !self.txn_alive() {
            return Write::Aborted;
        }
        let need = Self::like_need(post_id);
        if out.capacity() - out.len() < need {
            return Write::Space { need };
        }
        if !self.before_write() {
            return Write::Aborted;
        }
        let result = self.statement(8).run(|s| {
            s.bind(1, user_id)?;
            s.bind(2, post_id)?;
            s.done()
        });
        // SAFETY: sqlite3_changes is connection-local; reset/clear do not mutate it.
        let changes = unsafe { sqlite3_changes(self.db.as_ptr()) };
        if let Err(code) = result {
            return self.write_error(code);
        }
        if !self.txn_alive() {
            return Write::Aborted;
        }
        let already_liked = match changes {
            1 => false,
            0 => {
                let exists = self.statement(9).run(|s| {
                    s.bind(1, post_id)?;
                    if !s.step()? {
                        return Ok(false);
                    }
                    s.done()?;
                    Ok(true)
                });
                match exists {
                    Ok(false) => return Write::NotFound,
                    Ok(true) => true,
                    Err(code) => return self.write_error(code),
                }
            }
            _ => {
                self.txn = Txn::Failed;
                return Write::Aborted;
            }
        };
        if !self.txn_alive() {
            return Write::Aborted;
        }
        let base = out.len();
        let mut writer = BodyWriter::new(out);
        writer.put(b"{\"liked\":true,\"already_liked\":");
        writer.put(if already_liked { b"true" } else { b"false" });
        writer.put(b",\"post_id\":");
        // cast_unsigned states the readers' deliberate two's-complement u64 reading of a rowid.
        writer.number(post_id.cast_unsigned());
        writer.put(b"}");
        if writer.finish() != Read::Ready {
            out.truncate(base);
            self.txn = Txn::Failed;
            return Write::Aborted;
        }
        #[cfg(twelve_fault_inject)]
        {
            self.successful_writes = self.successful_writes.saturating_add(1);
        }
        Write::Liked { already_liked }
    }

    pub(crate) fn wal_frames(&self) -> u32 {
        self.hook.frames.get()
    }
    pub(crate) fn readers_opened(&self) -> bool {
        self.readers_opened
    }
    pub(crate) fn take_commit(&mut self) -> Option<Commit> {
        self.pending_commit.take()
    }
    pub(crate) fn take_counts(&mut self) -> StoreCounts {
        std::mem::take(&mut self.counts)
    }

    fn cache_value(&self, verb: c_int) -> Result<u64, c_int> {
        let (mut current, mut high) = (0, 0);
        // SAFETY: db is live; both writable i64 outputs match the pinned 64-bit ABI.
        check(unsafe { sqlite3_db_status64(self.db.as_ptr(), verb, &mut current, &mut high, 0) })?;
        u64::try_from(current).map_err(|_| ERROR)
    }

    pub(crate) fn cache_counts(&mut self) -> (u64, u64) {
        let hits = self
            .cache_value(7)
            .unwrap_or_else(|_| invariant("cache-status"));
        let misses = self
            .cache_value(8)
            .unwrap_or_else(|_| invariant("cache-status"));
        (hits, misses)
    }
}

fn selected_slot(id: i64, min: i64, slots: &[i8; 256]) -> Option<usize> {
    let at = usize::try_from(id.checked_sub(min)?).ok()?;
    usize::try_from(*slots.get(at)?).ok()
}

fn valid_timestamp(bytes: &[u8]) -> bool {
    bytes.len() == 24
        && bytes.iter().enumerate().all(|(at, byte)| match at {
            4 | 7 => *byte == b'-',
            10 => *byte == b'T',
            13 | 16 => *byte == b':',
            19 => *byte == b'.',
            23 => *byte == b'Z',
            _ => byte.is_ascii_digit(),
        })
}

fn database_message(db: NonNull<sys::sqlite3>) -> String {
    // SAFETY: db is live; SQLite returns its current NUL-terminated diagnostic, owned by db.
    let message = unsafe { sqlite3_errmsg(db.as_ptr()) };
    // SAFETY: the diagnostic stays valid until the next SQLite call on db, and it is copied here
    // before any statement reset or other SQLite operation can replace it.
    let message = unsafe { CStr::from_ptr(message) };
    match message.to_str() {
        Ok(text) => text.to_owned(),
        Err(_) => String::from("sqlite message not UTF-8"),
    }
}

/// # Safety
/// SQLite calls this after each commit with the data pointer registered by sqlite3_wal_hook, the
/// pinned HookState that Drop keeps alive until it removes the hook, the valid connection that
/// committed, a NUL-terminated database name and that database's WAL frame count.
unsafe extern "C" fn wal_callback(
    data: *mut c_void,
    _db: *mut sys::sqlite3,
    name: *const c_char,
    frames: c_int,
) -> c_int {
    // SAFETY: registration supplies the pinned HookState; removal precedes freeing the box, on
    // this same thread.
    let state = unsafe { &*data.cast::<HookState>() };
    // SAFETY: the callback name is a live SQLite C string for this invocation.
    let main = unsafe { CStr::from_ptr(name) }.to_bytes() == b"main";
    if let Ok(frames) = u32::try_from(frames)
        && main
    {
        state.frames.set(frames);
        state.wrote.set(true);
    }
    OK
}

impl Drop for Store {
    fn drop(&mut self) {
        // SAFETY: drop runs once and nothing uses the statements after this line; each finalizes
        // through its own Drop while db is still open.
        unsafe { ManuallyDrop::drop(&mut self.statements) };
        if self.readers_opened {
            // SAFETY: every statement is finalized and the readers' connection is still open.
            unsafe { twelve_readers_close() };
        }
        // SAFETY: db is live; the hook is removed before the HookState box, a field, drops after
        // this body.
        unsafe { sqlite3_wal_hook(self.db.as_ptr(), None, ptr::null_mut()) };
        #[cfg(twelve_fault_inject)]
        {
            // SAFETY: db is live; the commit hook is removed before the fault boxes drop.
            unsafe { sqlite3_commit_hook(self.db.as_ptr(), None, ptr::null_mut()) };
            // SAFETY: db is live; removing an authorizer that may be absent is a no-op.
            unsafe { sqlite3_set_authorizer(self.db.as_ptr(), None, ptr::null_mut()) };
        }
        if !self.autocommit() {
            let _ = Statement::prepare(self.db, SQL[13], false)
                .and_then(|mut s| s.run(Statement::done));
        }
        // SAFETY: every owned statement, cursor and callback is gone; the fault
        // VFS boxes remain live until the Store fields drop after this close.
        unsafe { sqlite3_close(self.db.as_ptr()) };
    }
}

#[cfg(twelve_fault_inject)]
mod fault {
    use super::check;
    use super::sys::{ERROR, NOMEM, OK};
    use std::ffi::{CStr, CString, c_char, c_int, c_void};
    use std::mem::{MaybeUninit, size_of};
    use std::ptr;
    use std::time::Duration;

    type SystemCall = Option<unsafe extern "C" fn()>;

    #[repr(C)]
    struct File {
        methods: *const IoMethods,
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct IoMethods {
        version: c_int,
        close: Option<unsafe extern "C" fn(*mut File) -> c_int>,
        read: Option<unsafe extern "C" fn(*mut File, *mut c_void, c_int, i64) -> c_int>,
        write: Option<unsafe extern "C" fn(*mut File, *const c_void, c_int, i64) -> c_int>,
        truncate: Option<unsafe extern "C" fn(*mut File, i64) -> c_int>,
        sync: Option<unsafe extern "C" fn(*mut File, c_int) -> c_int>,
        file_size: Option<unsafe extern "C" fn(*mut File, *mut i64) -> c_int>,
        lock: Option<unsafe extern "C" fn(*mut File, c_int) -> c_int>,
        unlock: Option<unsafe extern "C" fn(*mut File, c_int) -> c_int>,
        reserved: Option<unsafe extern "C" fn(*mut File, *mut c_int) -> c_int>,
        control: Option<unsafe extern "C" fn(*mut File, c_int, *mut c_void) -> c_int>,
        sector: Option<unsafe extern "C" fn(*mut File) -> c_int>,
        characteristics: Option<unsafe extern "C" fn(*mut File) -> c_int>,
        shm_map:
            Option<unsafe extern "C" fn(*mut File, c_int, c_int, c_int, *mut *mut c_void) -> c_int>,
        shm_lock: Option<unsafe extern "C" fn(*mut File, c_int, c_int, c_int) -> c_int>,
        shm_barrier: Option<unsafe extern "C" fn(*mut File)>,
        shm_unmap: Option<unsafe extern "C" fn(*mut File, c_int) -> c_int>,
        fetch: Option<unsafe extern "C" fn(*mut File, i64, c_int, *mut *mut c_void) -> c_int>,
        unfetch: Option<unsafe extern "C" fn(*mut File, i64, *mut c_void) -> c_int>,
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct Vfs {
        version: c_int,
        file_size: c_int,
        max_path: c_int,
        next: *mut Vfs,
        name: *const c_char,
        app_data: *mut c_void,
        open: Option<
            unsafe extern "C" fn(*mut Vfs, *const c_char, *mut File, c_int, *mut c_int) -> c_int,
        >,
        delete: Option<unsafe extern "C" fn(*mut Vfs, *const c_char, c_int) -> c_int>,
        access: Option<unsafe extern "C" fn(*mut Vfs, *const c_char, c_int, *mut c_int) -> c_int>,
        full_path:
            Option<unsafe extern "C" fn(*mut Vfs, *const c_char, c_int, *mut c_char) -> c_int>,
        dl_open: Option<unsafe extern "C" fn(*mut Vfs, *const c_char) -> *mut c_void>,
        dl_error: Option<unsafe extern "C" fn(*mut Vfs, c_int, *mut c_char)>,
        dl_sym: Option<unsafe extern "C" fn(*mut Vfs, *mut c_void, *const c_char) -> SystemCall>,
        dl_close: Option<unsafe extern "C" fn(*mut Vfs, *mut c_void)>,
        randomness: Option<unsafe extern "C" fn(*mut Vfs, c_int, *mut c_char) -> c_int>,
        sleep: Option<unsafe extern "C" fn(*mut Vfs, c_int) -> c_int>,
        current_time: Option<unsafe extern "C" fn(*mut Vfs, *mut f64) -> c_int>,
        last_error: Option<unsafe extern "C" fn(*mut Vfs, c_int, *mut c_char) -> c_int>,
        current_time64: Option<unsafe extern "C" fn(*mut Vfs, *mut i64) -> c_int>,
        set_system_call: Option<unsafe extern "C" fn(*mut Vfs, *const c_char, SystemCall) -> c_int>,
        get_system_call: Option<unsafe extern "C" fn(*mut Vfs, *const c_char) -> SystemCall>,
        next_system_call: Option<unsafe extern "C" fn(*mut Vfs, *const c_char) -> *const c_char>,
    }

    unsafe extern "C" {
        fn sqlite3_vfs_find(name: *const c_char) -> *mut Vfs;
        fn sqlite3_vfs_register(vfs: *mut Vfs, default: c_int) -> c_int;
        fn sqlite3_vfs_unregister(vfs: *mut Vfs) -> c_int;
    }

    pub(super) struct FaultState {
        real: *mut Vfs,
        paths: [CString; 6],
    }

    #[repr(C)]
    struct Header {
        base: File,
        real: *mut File,
        state: *const FaultState,
        methods: IoMethods,
    }

    const HEADER_SIZE: usize = (size_of::<Header>() + 7) & !7;

    pub(super) struct FaultVfs {
        vfs: Box<Vfs>,
        state: Box<FaultState>,
    }

    fn present(path: &CStr) -> bool {
        let mut metadata = MaybeUninit::<libc::stat>::uninit();
        // SAFETY: stat gets a terminated precomputed path and a writable native
        // output object. It follows links; no file content or output is read.
        unsafe { libc::stat(path.as_ptr(), metadata.as_mut_ptr()) == 0 }
    }

    impl FaultVfs {
        pub(super) fn new(path: &str) -> Result<Self, c_int> {
            // SAFETY: the pinned SQLite unit owns unix-excl for process lifetime.
            let real = unsafe { sqlite3_vfs_find(c"unix-excl".as_ptr()) };
            if real.is_null() {
                return Err(ERROR);
            }
            // SAFETY: this known VFS exposes the pinned complete public ABI.
            let mut vfs = Box::new(unsafe { *real });
            if !(1..=3).contains(&vfs.version) || vfs.open.is_none() || vfs.file_size < 0 {
                return Err(ERROR);
            }
            let suffixes = [
                "-fault-store-commit-fail",
                "-fault-store-commit-hold",
                "-fault-store-auto-rollback",
                "-fault-store-health-fail",
                "-fault-store-sync-fail",
                "-fault-store-sync-delay",
            ];
            let paths = suffixes.map(|suffix| {
                CString::new(format!("{path}{suffix}"))
                    .expect("the validated path and fixed suffix have no NUL")
            });
            let state = Box::new(FaultState { real, paths });
            vfs.file_size = vfs
                .file_size
                .checked_add(c_int::try_from(HEADER_SIZE).map_err(|_| NOMEM)?)
                .ok_or(NOMEM)?;
            vfs.next = ptr::null_mut();
            vfs.name = c"twelve-store-fault".as_ptr();
            vfs.app_data = (&*state as *const FaultState).cast_mut().cast();
            vfs.open = Some(open);
            replace(&mut vfs.delete, v_delete);
            replace(&mut vfs.access, v_access);
            replace(&mut vfs.full_path, v_full_path);
            replace(&mut vfs.dl_open, v_dl_open);
            replace(&mut vfs.dl_error, v_dl_error);
            replace(&mut vfs.dl_sym, v_dl_sym);
            replace(&mut vfs.dl_close, v_dl_close);
            replace(&mut vfs.randomness, v_randomness);
            replace(&mut vfs.sleep, v_sleep);
            replace(&mut vfs.current_time, v_current_time);
            replace(&mut vfs.last_error, v_last_error);
            if vfs.version >= 2 {
                replace(&mut vfs.current_time64, v_current_time64);
            }
            if vfs.version >= 3 {
                replace(&mut vfs.set_system_call, v_set_system_call);
                replace(&mut vfs.get_system_call, v_get_system_call);
                replace(&mut vfs.next_system_call, v_next_system_call);
            }
            // SAFETY: both boxes are pinned before registration and remain alive
            // through request-file close and VFS unregistration.
            check(unsafe { sqlite3_vfs_register(&mut *vfs, 0) })?;
            Ok(Self { vfs, state })
        }

        pub(super) fn data(&self) -> *mut c_void {
            (&*self.state as *const FaultState).cast_mut().cast()
        }

        pub(super) fn auto_rollback(&self) -> bool {
            present(&self.state.paths[2])
        }
        pub(super) fn health_failed(&self) -> bool {
            present(&self.state.paths[3])
        }
    }

    impl Drop for FaultVfs {
        fn drop(&mut self) {
            // SAFETY: Store closes every request file before this field drops.
            unsafe { sqlite3_vfs_unregister(&mut *self.vfs) };
        }
    }

    // Sets a slot only while it holds Some, so a NULL entry of the real table stays NULL.
    fn replace<T>(slot: &mut Option<T>, value: T) {
        if slot.is_some() {
            *slot = Some(value);
        }
    }

    /// # Safety
    /// SQLite calls this before each COMMIT with the data pointer registered by
    /// sqlite3_commit_hook, the pinned FaultState that Store keeps alive until it removes the hook.
    pub(super) unsafe extern "C" fn commit_callback(data: *mut c_void) -> c_int {
        // SAFETY: callback registration supplies the pinned state until removal.
        let state = unsafe { &*data.cast::<FaultState>() };
        if present(&state.paths[0]) {
            return 1;
        }
        while present(&state.paths[1]) {
            std::thread::sleep(Duration::from_millis(1));
        }
        c_int::from(present(&state.paths[0]))
    }

    /// # Safety
    /// SQLite calls this while it prepares a statement, with an action code and four text
    /// arguments that are NULL or NUL-terminated; none of them is read.
    pub(super) unsafe extern "C" fn health_authorizer(
        _data: *mut c_void,
        action: c_int,
        _a: *const c_char,
        _b: *const c_char,
        _db: *const c_char,
        _trigger: *const c_char,
    ) -> c_int {
        c_int::from(action == 21)
    }

    /// # Safety
    /// SQLite calls this with the registered shim VFS, whose app_data is the pinned FaultState; a
    /// writable file block of the registered szOsFile bytes, aligned for pointers; a name that is
    /// NULL or NUL-terminated; the open flags; and an out_flags that is NULL or writable.
    unsafe extern "C" fn open(
        vfs: *mut Vfs,
        name: *const c_char,
        file: *mut File,
        flags: c_int,
        out_flags: *mut c_int,
    ) -> c_int {
        // SAFETY: SQLite passes the registered shim VFS, whose app_data is the pinned FaultState.
        let state = unsafe { (*vfs).app_data }.cast::<FaultState>();
        // SAFETY: the pinned FaultState lives until the shim is unregistered.
        let real_vfs = unsafe { (*state).real };
        // SAFETY: the file block holds HEADER_SIZE header bytes and then the real file's
        // szOsFile bytes, so this offset stays inside the block.
        let real = unsafe { file.cast::<u8>().add(HEADER_SIZE) }.cast::<File>();
        // SAFETY: the block starts with a writable File; a NULL methods pointer tells SQLite
        // that no xClose is due if the real open fails.
        unsafe { (*file).methods = ptr::null() };
        // SAFETY: real_vfs is the live unix-excl VFS, whose table outlives the process's files.
        let Some(real_open) = (unsafe { (*real_vfs).open }) else {
            return ERROR;
        };
        // SAFETY: unix-excl's xOpen gets its own VFS, the caller's name, flags and out_flags
        // unchanged, and the real file's extent after the header.
        let code = unsafe { real_open(real_vfs, name, real, flags, out_flags) };
        if code != OK {
            return code;
        }
        // SAFETY: the successful real open initialized the real file.
        let real_methods = unsafe { (*real).methods };
        // SAFETY: a successful open sets a live methods table that outlives the file.
        let mut methods = unsafe { *real_methods };
        replace(&mut methods.close, f_close);
        replace(&mut methods.read, f_read);
        replace(&mut methods.write, f_write);
        replace(&mut methods.truncate, f_truncate);
        replace(&mut methods.sync, f_sync);
        replace(&mut methods.file_size, f_file_size);
        replace(&mut methods.lock, f_lock);
        replace(&mut methods.unlock, f_unlock);
        replace(&mut methods.reserved, f_reserved);
        replace(&mut methods.control, f_control);
        replace(&mut methods.sector, f_sector);
        replace(&mut methods.characteristics, f_characteristics);
        if methods.version >= 2 {
            replace(&mut methods.shm_map, f_shm_map);
            replace(&mut methods.shm_lock, f_shm_lock);
            replace(&mut methods.shm_barrier, f_shm_barrier);
            replace(&mut methods.shm_unmap, f_shm_unmap);
        }
        if methods.version >= 3 {
            replace(&mut methods.fetch, f_fetch);
            replace(&mut methods.unfetch, f_unfetch);
        }
        let header = file.cast::<Header>();
        // SAFETY: the block starts with HEADER_SIZE writable bytes aligned for pointers, room for
        // a Header; ptr::write drops no previous value.
        unsafe {
            ptr::write(
                header,
                Header {
                    base: File {
                        methods: ptr::null(),
                    },
                    real,
                    state,
                    methods,
                },
            );
        }
        // SAFETY: header holds the Header just written; taking a field's address reads nothing.
        let own_methods = unsafe { &raw const (*header).methods };
        // SAFETY: header holds the Header just written; its methods copy lives as long as the
        // file block, so SQLite may call through it until xClose.
        unsafe { (*header).base.methods = own_methods };
        OK
    }

    macro_rules! file_forward {
        ($name:ident, $field:ident, ($($arg:ident: $type:ty),*) $(-> $result:ty, $absent:expr)?) => {
            /// # Safety
            /// SQLite calls this with a file the shim's open initialized, whose header names the
            /// live real file, and with arguments valid for the real method it forwards to.
            unsafe extern "C" fn $name(file: *mut File, $($arg: $type),*) $(-> $result)? {
                // SAFETY: the file starts with the Header open wrote; it lives until xClose returns.
                let real = unsafe { (*file.cast::<Header>()).real };
                // SAFETY: the real file stays open and keeps its live methods table.
                let methods = unsafe { (*real).methods };
                // SAFETY: the methods table outlives the real file.
                match unsafe { (*methods).$field } {
                    // SAFETY: the real method gets the real file and the unchanged arguments.
                    Some(method) => unsafe { method(real, $($arg),*) },
                    None => { $($absent)? }
                }
            }
        };
    }

    file_forward!(f_close, close, () -> c_int, ERROR);
    file_forward!(f_read, read, (bytes: *mut c_void, length: c_int, offset: i64) -> c_int, ERROR);
    file_forward!(f_write, write, (bytes: *const c_void, length: c_int, offset: i64) -> c_int, ERROR);
    file_forward!(f_truncate, truncate, (size: i64) -> c_int, ERROR);
    file_forward!(f_file_size, file_size, (size: *mut i64) -> c_int, ERROR);
    file_forward!(f_lock, lock, (level: c_int) -> c_int, ERROR);
    file_forward!(f_unlock, unlock, (level: c_int) -> c_int, ERROR);
    file_forward!(f_reserved, reserved, (result: *mut c_int) -> c_int, ERROR);
    file_forward!(f_control, control, (op: c_int, argument: *mut c_void) -> c_int, ERROR);
    file_forward!(f_sector, sector, () -> c_int, ERROR);
    file_forward!(f_characteristics, characteristics, () -> c_int, ERROR);
    file_forward!(f_shm_map, shm_map, (page: c_int, size: c_int, extend: c_int, out: *mut *mut c_void) -> c_int, ERROR);
    file_forward!(f_shm_lock, shm_lock, (offset: c_int, count: c_int, flags: c_int) -> c_int, ERROR);
    file_forward!(f_shm_barrier, shm_barrier, ());
    file_forward!(f_shm_unmap, shm_unmap, (delete: c_int) -> c_int, ERROR);
    file_forward!(f_fetch, fetch, (offset: i64, size: c_int, out: *mut *mut c_void) -> c_int, ERROR);
    file_forward!(f_unfetch, unfetch, (offset: i64, page: *mut c_void) -> c_int, ERROR);

    /// # Safety
    /// SQLite calls this with a file the shim's open initialized, whose header names the live
    /// real file and the pinned FaultState, and with the sync flags for the real xSync.
    unsafe extern "C" fn f_sync(file: *mut File, flags: c_int) -> c_int {
        // SAFETY: the file starts with the Header open wrote; it lives until xClose returns.
        let header = unsafe { &*file.cast::<Header>() };
        // SAFETY: the header's state is the pinned FaultState, alive until the shim is
        // unregistered after every request file closed.
        let state = unsafe { &*header.state };
        if present(&state.paths[4]) {
            return 10 | (4 << 8);
        }
        while present(&state.paths[5]) {
            std::thread::sleep(Duration::from_millis(1));
        }
        // SAFETY: the real file stays open and keeps its live methods table.
        let methods = unsafe { (*header.real).methods };
        // SAFETY: the methods table outlives the real file.
        let Some(sync) = (unsafe { (*methods).sync }) else {
            return ERROR;
        };
        // SAFETY: the real xSync gets the real file and the original flags.
        unsafe { sync(header.real, flags) }
    }

    macro_rules! vfs_forward {
        ($name:ident, $field:ident, ($($arg:ident: $type:ty),*) $(-> $result:ty, $absent:expr)?) => {
            /// # Safety
            /// SQLite calls this with the registered shim VFS, whose app_data is the pinned
            /// FaultState, and with arguments valid for the real unix-excl method.
            unsafe extern "C" fn $name(vfs: *mut Vfs, $($arg: $type),*) $(-> $result)? {
                // SAFETY: SQLite passes the registered shim VFS.
                let data = unsafe { (*vfs).app_data };
                // SAFETY: the shim VFS's app_data is the pinned FaultState.
                let state = unsafe { &*data.cast::<FaultState>() };
                // SAFETY: state.real is the live unix-excl VFS, whose table outlives every file.
                match unsafe { (*state.real).$field } {
                    // SAFETY: the real method gets the original VFS, never the wrapper, and the
                    // unchanged arguments.
                    Some(method) => unsafe { method(state.real, $($arg),*) },
                    None => { $($absent)? }
                }
            }
        };
    }

    vfs_forward!(v_delete, delete, (name: *const c_char, sync: c_int) -> c_int, ERROR);
    vfs_forward!(v_access, access, (name: *const c_char, flags: c_int, out: *mut c_int) -> c_int, ERROR);
    vfs_forward!(v_full_path, full_path, (name: *const c_char, size: c_int, out: *mut c_char) -> c_int, ERROR);
    vfs_forward!(v_dl_open, dl_open, (name: *const c_char) -> *mut c_void, ptr::null_mut());
    vfs_forward!(v_dl_error, dl_error, (size: c_int, out: *mut c_char));
    vfs_forward!(v_dl_sym, dl_sym, (library: *mut c_void, name: *const c_char) -> SystemCall, None);
    vfs_forward!(v_dl_close, dl_close, (library: *mut c_void));
    vfs_forward!(v_randomness, randomness, (size: c_int, out: *mut c_char) -> c_int, ERROR);
    vfs_forward!(v_sleep, sleep, (us: c_int) -> c_int, ERROR);
    vfs_forward!(v_current_time, current_time, (out: *mut f64) -> c_int, ERROR);
    vfs_forward!(v_last_error, last_error, (size: c_int, out: *mut c_char) -> c_int, ERROR);
    vfs_forward!(v_current_time64, current_time64, (out: *mut i64) -> c_int, ERROR);
    vfs_forward!(v_set_system_call, set_system_call, (name: *const c_char, call: SystemCall) -> c_int, ERROR);
    vfs_forward!(v_get_system_call, get_system_call, (name: *const c_char) -> SystemCall, None);
    vfs_forward!(v_next_system_call, next_system_call, (name: *const c_char) -> *const c_char, ptr::null());
}
