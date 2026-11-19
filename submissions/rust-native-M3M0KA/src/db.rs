//! SQLite access through its C API, plus the optional B-tree read plans.
//!
//! One connection, one thread, prepared statements created once. Every response
//! is serialized from rows read while its request is being served; nothing is
//! cached across requests. WAL + `synchronous=NORMAL` keeps a commit durable
//! before the response that reports it is allowed to leave. This is application-
//! crash durability, not a promise against OS crash or power loss.

use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crate::btree::Cursors;
use crate::http::{put_i64, put_jstr};

#[repr(C)]
pub struct Sqlite3 {
    _private: [u8; 0],
}

#[repr(C)]
pub struct Stmt {
    _private: [u8; 0],
}

const SQLITE_OK: c_int = 0;
const SQLITE_ROW: c_int = 100;
const SQLITE_DONE: c_int = 101;
const SQLITE_LIMIT_LENGTH: c_int = 0; // pinned sqlite3.h runtime limit category
const MATERIALIZED_VALUE_MAX: c_int = 32 * 1024 * 1024;
const SQLITE_OPEN_READWRITE: c_int = 0x0000_0002;
const SQLITE_CHECKPOINT_PASSIVE: c_int = 0;
const SQLITE_CHECKPOINT_RESTART: c_int = 2;
const DEFAULT_WAL_CKPT_PAGES: u64 = 4096;
const EMERGENCY_WAL_CKPT_PAGES: u64 = 65_536;
const WAL_RESTART_COOLDOWN: Duration = Duration::from_secs(2);

extern "C" {
    fn sqlite3_open_v2(
        filename: *const c_char,
        db: *mut *mut Sqlite3,
        flags: c_int,
        vfs: *const c_char,
    ) -> c_int;
    fn sqlite3_close(db: *mut Sqlite3) -> c_int;
    fn sqlite3_close_v2(db: *mut Sqlite3) -> c_int;
    fn sqlite3_errmsg(db: *mut Sqlite3) -> *const c_char;
    fn sqlite3_exec(
        db: *mut Sqlite3,
        sql: *const c_char,
        cb: *const c_void,
        arg: *mut c_void,
        err: *mut *mut c_char,
    ) -> c_int;
    fn sqlite3_free(p: *mut c_void);
    fn sqlite3_busy_timeout(db: *mut Sqlite3, ms: c_int) -> c_int;
    fn sqlite3_limit(db: *mut Sqlite3, category: c_int, value: c_int) -> c_int;
    fn sqlite3_prepare_v3(
        db: *mut Sqlite3,
        sql: *const c_char,
        n: c_int,
        flags: u32,
        stmt: *mut *mut Stmt,
        tail: *mut *const c_char,
    ) -> c_int;
    fn sqlite3_finalize(s: *mut Stmt) -> c_int;
    fn sqlite3_bind_int64(s: *mut Stmt, i: c_int, v: i64) -> c_int;
    fn sqlite3_bind_text(
        s: *mut Stmt,
        i: c_int,
        p: *const c_char,
        n: c_int,
        d: Option<unsafe extern "C" fn(*mut c_void)>,
    ) -> c_int;
    fn sqlite3_step(s: *mut Stmt) -> c_int;
    fn sqlite3_reset(s: *mut Stmt) -> c_int;
    fn sqlite3_clear_bindings(s: *mut Stmt) -> c_int;
    fn feed_prepare_persistent() -> u32;
    fn sqlite3_column_int64(s: *mut Stmt, i: c_int) -> i64;
    fn sqlite3_column_text(s: *mut Stmt, i: c_int) -> *const c_char;
    fn sqlite3_column_bytes(s: *mut Stmt, i: c_int) -> c_int;
    fn sqlite3_changes(db: *mut Sqlite3) -> c_int;
    fn sqlite3_get_autocommit(db: *mut Sqlite3) -> c_int;
    fn sqlite3_txn_state(db: *mut Sqlite3, schema: *const c_char) -> c_int;
    fn sqlite3_wal_checkpoint_v2(
        db: *mut Sqlite3,
        z_db: *const c_char,
        e_mode: c_int,
        pn_log: *mut c_int,
        pn_ckpt: *mut c_int,
    ) -> c_int;
    fn feed_install_wal_counter(db: *mut Sqlite3, pages_out: *mut u64);
    fn nice(inc: c_int) -> c_int;
}

struct WalState {
    wal_pages: AtomicU64,
    restart_requested: AtomicBool,
    write_active: AtomicBool,
    shutdown: AtomicBool,
    passive_ok: AtomicU64,
    restart_ok: AtomicU64,
    restart_busy: AtomicU64,
    mu: Mutex<()>,
    cv: Condvar,
}

struct WalCheckpointer {
    state: Arc<WalState>,
    ckpt_pages: u64,
    handle: Option<JoinHandle<()>>,
}

impl WalCheckpointer {
    fn start(path: &str, main_db: *mut Sqlite3, ckpt_pages: u64) -> Option<Self> {
        let cpath = CString::new(path).ok()?;
        let ckpt_pages = ckpt_pages.max(1);
        let state = Arc::new(WalState {
            wal_pages: AtomicU64::new(0),
            restart_requested: AtomicBool::new(false),
            write_active: AtomicBool::new(false),
            shutdown: AtomicBool::new(false),
            passive_ok: AtomicU64::new(0),
            restart_ok: AtomicU64::new(0),
            restart_busy: AtomicU64::new(0),
            mu: Mutex::new(()),
            cv: Condvar::new(),
        });
        unsafe {
            feed_install_wal_counter(main_db, state.wal_pages.as_ptr());
        }
        let thread_state = Arc::clone(&state);
        let handle = std::thread::Builder::new()
            .name(String::from("wal-ckpt"))
            .spawn(move || Self::worker(cpath, thread_state, ckpt_pages))
            .ok()?;
        Some(Self {
            state,
            ckpt_pages,
            handle: Some(handle),
        })
    }

    fn worker(cpath: CString, state: Arc<WalState>, ckpt_pages: u64) {
        unsafe {
            let _ = nice(19);
        }
        let mut ckpt_h: *mut Sqlite3 = std::ptr::null_mut();
        let rc = unsafe {
            sqlite3_open_v2(
                cpath.as_ptr(),
                &mut ckpt_h,
                SQLITE_OPEN_READWRITE,
                std::ptr::null(),
            )
        };
        if rc != SQLITE_OK || ckpt_h.is_null() {
            eprintln!("wal-ckpt: open failed ({rc})");
            return;
        }
        unsafe {
            sqlite3_busy_timeout(ckpt_h, 50);
            let setup = CString::new(
                "PRAGMA temp_store = MEMORY; PRAGMA mmap_size = 0; PRAGMA cache_size = -512;",
            )
            .unwrap();
            let _ = sqlite3_exec(
                ckpt_h,
                setup.as_ptr(),
                std::ptr::null(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            );
        }

        let mut last_restart: Option<Instant> = None;
        let mut last_passive_pages: u64 = 0;
        let main_c = b"main\0".as_ptr() as *const c_char;

        while !state.shutdown.load(Ordering::Acquire) {
            {
                let mut guard = state.mu.lock().unwrap();
                loop {
                    if state.shutdown.load(Ordering::Acquire) {
                        break;
                    }
                    let pages = state.wal_pages.load(Ordering::Acquire);
                    if pages >= EMERGENCY_WAL_CKPT_PAGES {
                        break;
                    }
                    let cooldown_rem = last_restart
                        .and_then(|t| WAL_RESTART_COOLDOWN.checked_sub(t.elapsed()));
                    if cooldown_rem.is_none() && pages >= ckpt_pages {
                        break;
                    }
                    if pages >= last_passive_pages.saturating_add(ckpt_pages) {
                        break;
                    }
                    guard = match cooldown_rem {
                        Some(rem) if pages >= ckpt_pages => {
                            let (g, _) = state.cv.wait_timeout(guard, rem).unwrap();
                            g
                        }
                        _ => state.cv.wait(guard).unwrap(),
                    };
                }
            }
            if state.shutdown.load(Ordering::Acquire) {
                break;
            }

            let mut n_log: c_int = 0;
            let mut n_ckpt: c_int = 0;
            let rc = unsafe {
                sqlite3_wal_checkpoint_v2(
                    ckpt_h,
                    main_c,
                    SQLITE_CHECKPOINT_PASSIVE,
                    &mut n_log,
                    &mut n_ckpt,
                )
            };
            if rc == SQLITE_OK {
                state.passive_ok.fetch_add(1, Ordering::Relaxed);
            }
            let pages_now = state.wal_pages.load(Ordering::Acquire);
            last_passive_pages = pages_now;

            let cooldown_ok = last_restart
                .map_or(true, |t| t.elapsed() >= WAL_RESTART_COOLDOWN);
            let emergency = pages_now >= EMERGENCY_WAL_CKPT_PAGES;
            if (pages_now >= ckpt_pages && cooldown_ok) || emergency {
                state.restart_requested.store(true, Ordering::Release);
                if state.write_active.load(Ordering::Acquire) {
                    let guard = state.mu.lock().unwrap();
                    let _ = state.cv.wait_timeout_while(
                        guard,
                        Duration::from_millis(50),
                        |_| {
                            state.write_active.load(Ordering::Acquire)
                                && !state.shutdown.load(Ordering::Acquire)
                        },
                    );
                }
                if !state.shutdown.load(Ordering::Acquire) {
                    last_restart = Some(Instant::now());
                    let mut r_log: c_int = 0;
                    let mut r_ckpt: c_int = 0;
                    let rc = unsafe {
                        sqlite3_wal_checkpoint_v2(
                            ckpt_h,
                            main_c,
                            SQLITE_CHECKPOINT_RESTART,
                            &mut r_log,
                            &mut r_ckpt,
                        )
                    };
                    if rc == SQLITE_OK {
                        state.wal_pages.store(0, Ordering::Release);
                        last_passive_pages = 0;
                        state.restart_ok.fetch_add(1, Ordering::Relaxed);
                    } else {
                        state.restart_busy.fetch_add(1, Ordering::Relaxed);
                    }
                }
                state.restart_requested.store(false, Ordering::Release);
                state.cv.notify_all();
            }
        }

        unsafe {
            let mut close_rc = sqlite3_close(ckpt_h);
            if close_rc != SQLITE_OK {
                close_rc = sqlite3_close_v2(ckpt_h);
            }
            if close_rc != SQLITE_OK {
                eprintln!("wal-ckpt: close failed ({close_rc})");
            }
        }
    }

    fn stop(&mut self) {
        self.state.shutdown.store(true, Ordering::Release);
        {
            let _guard = self.state.mu.lock().unwrap();
            self.state.cv.notify_all();
        }
        if let Some(h) = self.handle.take() {
            let _ = h.join();
        }
    }
}

pub enum LikeResult {
    Created,
    AlreadyLiked,
    Missing,
    Error,
}

unsafe fn column(s: *mut Stmt, i: c_int) -> &'static [u8] {
    let p = sqlite3_column_text(s, i);
    let n = sqlite3_column_bytes(s, i);
    if p.is_null() || n <= 0 {
        &[]
    } else {
        std::slice::from_raw_parts(p as *const u8, n as usize)
    }
}

fn row_json(s: *mut Stmt, out: &mut impl crate::buffer::Output) {
    unsafe {
        out.extend_from_slice(b"{\"id\":");
        put_i64(out, sqlite3_column_int64(s, 0));
        out.extend_from_slice(b",\"body\":");
        put_jstr(out, column(s, 1));
        out.extend_from_slice(b",\"created_at\":");
        put_jstr(out, column(s, 2));
        out.extend_from_slice(b",\"author\":");
        put_jstr(out, column(s, 3));
        out.extend_from_slice(b",\"like_count\":");
        put_i64(out, sqlite3_column_int64(s, 4));
        out.push(b'}');
    }
}

const STMT_FEED: u16 = 1 << 0;
const STMT_GET: u16 = 1 << 1;
const STMT_EXISTS: u16 = 1 << 2;
const STMT_INSERT: u16 = 1 << 3;
const STMT_LIKE: u16 = 1 << 4;
const STMT_PING: u16 = 1 << 5;
const STMT_ROOT: u16 = 1 << 6;
const STMT_BEGIN: u16 = 1 << 7;
const STMT_BEGIN_IMM: u16 = 1 << 8;
const STMT_COMMIT: u16 = 1 << 9;
const STMT_ROLLBACK: u16 = 1 << 10;

pub struct Db {
    h: *mut Sqlite3,
    q_feed: *mut Stmt,
    q_get: *mut Stmt,
    q_exists: *mut Stmt,
    q_insert: *mut Stmt,
    q_like: *mut Stmt,
    q_ping: *mut Stmt,
    q_root: *mut Stmt,
    q_begin: *mut Stmt,
    q_begin_imm: *mut Stmt,
    q_commit: *mut Stmt,
    q_rollback: *mut Stmt,
    dirty_stmts: u16,
    in_batch: bool,
    batch_failed: bool,
    broken: bool,
    plan: Option<Cursors>,
    plan_ok: bool,
    checkpointer: Option<WalCheckpointer>,
}

impl Db {
    pub fn open_with_cache(path: &str, cache_kb: i32) -> Db {
        let cpath = CString::new(path).expect("path");
        let mut h: *mut Sqlite3 = std::ptr::null_mut();
        unsafe {
            if sqlite3_open_v2(
                cpath.as_ptr(),
                &mut h,
                SQLITE_OPEN_READWRITE,
                std::ptr::null(),
            ) != SQLITE_OK
            {
                panic!("cannot open {path}");
            }
            // OP_Column checks this before allocating an overflow TEXT value.
            // The output quota alone acts too late to bound SQLite materialization.
            // This is an overload limit, not a schema or SQLite-engine change.
            sqlite3_limit(h, SQLITE_LIMIT_LENGTH, MATERIALIZED_VALUE_MAX);
            sqlite3_busy_timeout(h, 5000);
        }
        let mut db = Db {
            h,
            q_feed: std::ptr::null_mut(),
            q_get: std::ptr::null_mut(),
            q_exists: std::ptr::null_mut(),
            q_insert: std::ptr::null_mut(),
            q_like: std::ptr::null_mut(),
            q_ping: std::ptr::null_mut(),
            q_root: std::ptr::null_mut(),
            q_begin: std::ptr::null_mut(),
            q_begin_imm: std::ptr::null_mut(),
            q_commit: std::ptr::null_mut(),
            q_rollback: std::ptr::null_mut(),
            dirty_stmts: 0,
            in_batch: false,
            batch_failed: false,
            broken: false,
            plan: None,
            plan_ok: false,
            checkpointer: None,
        };
        // A PASSIVE checkpoint copies the accumulated frames into the database
        // file inside the statement that crosses the threshold. Smaller means a
        // shorter stall but more often; the total bytes copied are the same.
        let ac: i64 = std::env::var("FEED_WAL_AC")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(512);
        for pragma in [
            String::from("PRAGMA journal_mode = WAL"),
            String::from("PRAGMA synchronous = NORMAL"),
            String::from("PRAGMA temp_store = MEMORY"),
            format!("PRAGMA cache_size = -{cache_kb}"),
            String::from("PRAGMA mmap_size = 0"),
            format!("PRAGMA wal_autocheckpoint = {ac}"),
        ] {
            db.exec(&pragma);
        }
        const COLS: &str = "SELECT p.id, p.body, p.created_at, u.username, \
             (SELECT count(*) FROM likes l WHERE l.post_id = p.id) \
             FROM posts p JOIN users u ON u.id = p.user_id";
        db.q_feed = db.prepare(&format!(
            "{COLS} ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
        ));
        db.q_get = db.prepare(&format!("{COLS} WHERE p.id = ?1"));
        db.q_exists = db.prepare("SELECT 1 FROM posts WHERE id = ?1");
        db.q_insert = db
            .prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
        db.q_like = db.prepare(
            "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) \
             ON CONFLICT (user_id, post_id) DO NOTHING",
        );
        db.q_ping = db.prepare("SELECT 1");
        db.q_root = db.prepare("SELECT rootpage FROM sqlite_schema WHERE name = ?1");
        db.q_begin = db.prepare("BEGIN");
        db.q_begin_imm = db.prepare("BEGIN IMMEDIATE");
        db.q_commit = db.prepare("COMMIT");
        db.q_rollback = db.prepare("ROLLBACK");
        db.init_plan();
        db
    }

    fn exec(&self, sql: &str) {
        let csql = CString::new(sql).expect("sql");
        let mut err: *mut c_char = std::ptr::null_mut();
        unsafe {
            if sqlite3_exec(
                self.h,
                csql.as_ptr(),
                std::ptr::null(),
                std::ptr::null_mut(),
                &mut err,
            ) != SQLITE_OK
            {
                let msg = if err.is_null() {
                    String::from("?")
                } else {
                    std::ffi::CStr::from_ptr(err).to_string_lossy().into_owned()
                };
                sqlite3_free(err as *mut c_void);
                panic!("sqlite: {sql}: {msg}");
            }
        }
    }

    fn prepare(&self, sql: &str) -> *mut Stmt {
        let csql = CString::new(sql).expect("sql");
        let mut s: *mut Stmt = std::ptr::null_mut();
        unsafe {
            if sqlite3_prepare_v3(
                self.h,
                csql.as_ptr(),
                -1,
                feed_prepare_persistent(),
                &mut s,
                std::ptr::null_mut(),
            ) != SQLITE_OK
            {
                let msg = std::ffi::CStr::from_ptr(sqlite3_errmsg(self.h)).to_string_lossy();
                panic!("prepare: {sql}: {msg}");
            }
        }
        s
    }

    fn root(&mut self, name: &str) -> Option<u32> {
        self.dirty_stmts |= STMT_ROOT;
        unsafe {
            let bound = sqlite3_bind_text(
                self.q_root,
                1,
                name.as_ptr() as *const c_char,
                name.len() as c_int,
                None,
            ) == SQLITE_OK;
            let rc = if bound { sqlite3_step(self.q_root) } else { -1 };
            let v = if rc == SQLITE_ROW {
                sqlite3_column_int64(self.q_root, 0) as u32
            } else {
                0
            };
            let reset = sqlite3_reset(self.q_root);
            let clear = sqlite3_clear_bindings(self.q_root);
            if reset == SQLITE_OK && clear == SQLITE_OK {
                self.dirty_stmts &= !STMT_ROOT;
            }
            if v != 0 && reset == SQLITE_OK && clear == SQLITE_OK {
                Some(v)
            } else {
                None
            }
        }
    }

    fn layout(&self, pragma: &str, name_col: c_int) -> String {
        let s = self.prepare(pragma);
        let mut out = String::new();
        unsafe {
            while sqlite3_step(s) == SQLITE_ROW {
                if !out.is_empty() {
                    out.push(',');
                }
                out.push_str(&String::from_utf8_lossy(column(s, name_col)));
            }
            sqlite3_finalize(s);
        }
        out
    }

    fn table_layout(&self, table: &str) -> String {
        let s = self.prepare(&format!("PRAGMA table_info(\"{table}\")"));
        let mut out = String::new();
        unsafe {
            while sqlite3_step(s) == SQLITE_ROW {
                if !out.is_empty() {
                    out.push(',');
                }
                out.push_str(&String::from_utf8_lossy(column(s, 1)));
                out.push(':');
                out.push_str(&String::from_utf8_lossy(column(s, 2)));
                out.push(':');
                out.push_str(&sqlite3_column_int64(s, 5).to_string());
            }
            sqlite3_finalize(s);
        }
        out
    }

    /// Key columns of an index in key order, with 1 = descending.
    fn index_layout(&self, index: &str) -> String {
        let s = self.prepare(&format!("PRAGMA index_xinfo(\"{index}\")"));
        let mut out = String::new();
        unsafe {
            while sqlite3_step(s) == SQLITE_ROW {
                if sqlite3_column_int64(s, 5) != 1 {
                    continue;
                }
                if !out.is_empty() {
                    out.push(',');
                }
                out.push_str(&String::from_utf8_lossy(column(s, 2)));
                out.push(':');
                out.push(if sqlite3_column_int64(s, 3) != 0 {
                    '1'
                } else {
                    '0'
                });
            }
            sqlite3_finalize(s);
        }
        out
    }

    fn schema_matches(&self) -> bool {
        let users = self.table_layout("users");
        let posts = self.table_layout("posts");
        let likes = self.table_layout("likes");
        let idx = self.layout("PRAGMA index_info(\"likes_post_id_idx\")", 2);
        let feed_idx = self.index_layout("posts_created_at_id_idx");
        let ok = users == "id:INTEGER:1,username:TEXT:0,created_at:TEXT:0"
            && posts == "id:INTEGER:1,user_id:INTEGER:0,body:TEXT:0,created_at:TEXT:0"
            && likes == "user_id:INTEGER:1,post_id:INTEGER:2,created_at:TEXT:0"
            && idx == "post_id"
            && feed_idx == "created_at:1,id:1";
        if !ok {
            eprintln!(
                "schema layout: users=[{users}] posts=[{posts}] likes=[{likes}] idx=[{idx}] \
                 feed_idx=[{feed_idx}]"
            );
        }
        ok
    }

    /// Decides once, at startup, whether the B-tree read plans are usable. The
    /// plan's output is compared byte for byte with the reference SQL here, so a
    /// layout this code does not understand is caught before any client sees it.
    fn init_plan(&mut self) {
        if std::env::var("FEED_NO_PLAN").is_ok() {
            eprintln!("read plans: disabled by FEED_NO_PLAN");
            return;
        }
        if !self.schema_matches() {
            eprintln!("schema differs from schema.sql: read plans disabled");
            return;
        }
        let (rp, ru, rl, rf) = match (
            self.root("posts"),
            self.root("users"),
            self.root("likes_post_id_idx"),
            self.root("posts_created_at_id_idx"),
        ) {
            (Some(a), Some(b), Some(c), Some(d)) => (a, b, c, d),
            _ => {
                eprintln!("missing b-tree roots: read plans disabled");
                return;
            }
        };
        let ki = unsafe { feed_keyinfo_shim(self.h, 1, 1) };
        let ki_newest = unsafe { feed_keyinfo_desc_shim(self.h, 2, 1) };
        if ki.is_null() || ki_newest.is_null() {
            eprintln!("keyinfo allocation failed: read plans disabled");
            unsafe {
                feed_keyinfo_release(ki);
                feed_keyinfo_release(ki_newest);
            }
            return;
        }
        let mut c = Cursors::new(self.h as *mut c_void, (rp, ru, rl, rf), ki, ki_newest);
        self.batch_begin(false);
        if self.batch_failed || c.open().is_none() {
            eprintln!("cursor setup failed: read plans disabled");
            self.batch_failed = true;
            self.batch_commit();
            return;
        }
        let mut planned = crate::buffer::Buffer::new(32 * 1024 * 1024);
        let planned_res = c.feed_json(&mut planned);
        let planned_ok = planned_res.is_ok() && !planned.failed();
        c.close();

        let mut reference = crate::buffer::Buffer::new(32 * 1024 * 1024);
        let sql_ok = self.feed_sql(&mut reference) && !reference.failed();
        let boundary_ok = self.batch_commit();
        let agree = if planned_ok {
            if sql_ok && planned.as_slice() != reference.as_slice() {
                let at = planned
                    .iter()
                    .zip(reference.iter())
                    .position(|(a, b)| a != b)
                    .unwrap_or_else(|| planned.len().min(reference.len()));
                let lo = at.saturating_sub(40);
                eprintln!(
                    "plan/SQL differ at {at} ({} vs {} bytes)\n plan: {}\n sql : {}",
                    planned.len(),
                    reference.len(),
                    String::from_utf8_lossy(&planned[lo..(at + 60).min(planned.len())]),
                    String::from_utf8_lossy(&reference[lo..(at + 60).min(reference.len())])
                );
            }
            sql_ok && boundary_ok && planned.as_slice() == reference.as_slice()
        } else {
            eprintln!("feed plan self check failed: {planned_res:?}");
            false
        };
        if !agree {
            eprintln!("feed plan disagrees with SQL: read plans disabled");
            return;
        }
        self.plan_ok = true;
        eprintln!(
            "read plans: enabled (posts_created_at_id_idx order, likes_post_id_idx counts, \
             verified against SQL)"
        );
        self.plan = Some(c);
    }

    /// Cursors must be closed before a write statement runs, and reopened for
    /// the reads that follow it in the same transaction.
    fn plan_reopen(&mut self) {
        if !self.plan_ok {
            return;
        }
        let mut failed = false;
        if let Some(p) = self.plan.as_mut() {
            if !p.is_open() && p.open().is_none() {
                failed = true;
            }
        } else {
            failed = true;
        }
        if failed {
            eprintln!("cursor reopen failed: read plans disabled");
            self.plan_ok = false;
        }
    }

    fn plan_close(&mut self) {
        if let Some(p) = self.plan.as_mut() {
            p.close();
        }
    }

    fn exec_rc(&self, sql: &str) -> c_int {
        let sql = CString::new(sql).expect("sql");
        unsafe {
            sqlite3_exec(
                self.h,
                sql.as_ptr(),
                std::ptr::null(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            )
        }
    }

    fn statements(&self) -> [(u16, *mut Stmt); 11] {
        [
            (STMT_FEED, self.q_feed),
            (STMT_GET, self.q_get),
            (STMT_EXISTS, self.q_exists),
            (STMT_INSERT, self.q_insert),
            (STMT_LIKE, self.q_like),
            (STMT_PING, self.q_ping),
            (STMT_ROOT, self.q_root),
            (STMT_BEGIN, self.q_begin),
            (STMT_BEGIN_IMM, self.q_begin_imm),
            (STMT_COMMIT, self.q_commit),
            (STMT_ROLLBACK, self.q_rollback),
        ]
    }

    fn run_txn_stmt(&mut self, bit: u16, s: *mut Stmt) -> c_int {
        self.dirty_stmts |= bit;
        let step = unsafe { sqlite3_step(s) };
        let reset = unsafe { sqlite3_reset(s) };
        if reset == SQLITE_OK {
            self.dirty_stmts &= !bit;
        }
        if step == SQLITE_DONE || step == SQLITE_ROW {
            reset
        } else {
            step
        }
    }

    fn idle(&self) -> bool {
        if self.h.is_null() {
            return true;
        }
        unsafe {
            sqlite3_get_autocommit(self.h) != 0
                && sqlite3_txn_state(self.h, b"main\0".as_ptr() as *const c_char) == 0
        }
    }

    // A deferred SQL BEGIN has txn_state NONE until its first read. SQL's
    // ownership flag must nevertheless be false before any custom read/write.
    fn ready(&mut self) -> bool {
        if self.broken || self.batch_failed || !self.in_batch {
            return false;
        }
        if unsafe { sqlite3_get_autocommit(self.h) } != 0 {
            self.batch_failed = true;
            self.plan_close();
            return false;
        }
        true
    }

    fn finish_statement(&mut self, bit: u16, s: *mut Stmt, success: bool) -> bool {
        let reset = unsafe { sqlite3_reset(s) };
        let clear = unsafe { sqlite3_clear_bindings(s) };
        if reset == SQLITE_OK && clear == SQLITE_OK {
            self.dirty_stmts &= !bit;
        }
        // Reset releases execution state, clear releases SQLITE_STATIC borrows.
        // A normal statement error (e.g. CHECK/ABORT) can leave the batch live.
        // An automatic rollback invalidates ALL provisional batch responses.
        let live = self.ready();
        if clear != SQLITE_OK {
            self.batch_failed = true;
        }
        success && reset == SQLITE_OK && clear == SQLITE_OK && live
    }

    fn cleanup(&mut self) -> bool {
        self.plan_close();
        let mut clean = true;
        if self.dirty_stmts != 0 {
            for (bit, s) in self.statements() {
                if (self.dirty_stmts & bit) != 0 && !s.is_null() {
                    // reset may repeat an already handled statement error. Its
                    // execution state is reset even then; clear must still run.
                    unsafe {
                        sqlite3_reset(s);
                    }
                    let clear_ok = unsafe { sqlite3_clear_bindings(s) } == SQLITE_OK;
                    if clear_ok {
                        self.dirty_stmts &= !bit;
                    }
                    clean &= clear_ok;
                }
            }
        }
        if !self.idle() {
            clean &= if !self.q_rollback.is_null() {
                self.run_txn_stmt(STMT_ROLLBACK, self.q_rollback) == SQLITE_OK
            } else {
                self.exec_rc("ROLLBACK") == SQLITE_OK
            };
        }
        clean && self.idle()
    }

    pub fn ping(&mut self) -> bool {
        if !self.ready() {
            return false;
        }
        self.dirty_stmts |= STMT_PING;
        let rc = unsafe { sqlite3_step(self.q_ping) };
        self.finish_statement(STMT_PING, self.q_ping, rc == SQLITE_ROW)
    }

    fn feed_sql(&mut self, out: &mut impl crate::buffer::Output) -> bool {
        if !self.ready() {
            return false;
        }
        let mark = out.len();
        out.extend_from_slice(b"{\"posts\":[");
        self.dirty_stmts |= STMT_FEED;
        let mut first = true;
        loop {
            let rc = unsafe { sqlite3_step(self.q_feed) };
            if rc == SQLITE_ROW {
                if !first {
                    out.push(b',');
                }
                first = false;
                row_json(self.q_feed, out);
                if out.failed() {
                    self.finish_statement(STMT_FEED, self.q_feed, false);
                    out.truncate(mark);
                    return false;
                }
                continue;
            }
            if !self.finish_statement(STMT_FEED, self.q_feed, rc == SQLITE_DONE) {
                out.truncate(mark);
                return false;
            }
            break;
        }
        out.extend_from_slice(b"]}");
        true
    }

    pub fn feed_json(&mut self, out: &mut impl crate::buffer::Output) -> bool {
        if !self.ready() {
            return false;
        }
        if self.plan_ok {
            self.plan_reopen();
        }
        if self.plan_ok {
            let mark = out.len();
            let mut done: Option<Result<(), &'static str>> = None;
            if let Some(p) = self.plan.as_mut() {
                done = Some(p.feed_json(out));
            }
            match done {
                Some(Ok(())) => return true,
                Some(Err(why)) => {
                    out.truncate(mark);
                    eprintln!("feed plan stopped ({why}): falling back to SQL");
                    self.plan_ok = false;
                }
                None => self.plan_ok = false,
            }
        }
        self.plan_close();
        self.feed_sql(out)
    }

    fn post_sql(&mut self, id: i64, out: &mut impl crate::buffer::Output) -> i32 {
        if !self.ready() {
            return -1;
        }
        let mark = out.len();
        self.dirty_stmts |= STMT_GET;
        let bound = unsafe { sqlite3_bind_int64(self.q_get, 1, id) } == SQLITE_OK;
        let rc = if bound {
            unsafe { sqlite3_step(self.q_get) }
        } else {
            -1
        };
        if rc == SQLITE_ROW {
            out.extend_from_slice(b"{\"post\":");
            row_json(self.q_get, out);
            out.push(b'}');
        }
        if !self.finish_statement(STMT_GET, self.q_get, rc == SQLITE_ROW || rc == SQLITE_DONE) {
            out.truncate(mark);
            return -1;
        }
        if rc == SQLITE_ROW {
            0
        } else {
            1
        }
    }

    pub fn post_json(&mut self, id: i64, out: &mut impl crate::buffer::Output) -> i32 {
        if !self.ready() {
            return -1;
        }
        if self.plan_ok {
            self.plan_reopen();
        }
        if self.plan_ok {
            let mut res = None;
            if let Some(p) = self.plan.as_mut() {
                res = Some(p.post_json(id, out));
            }
            match res {
                Some(0) => return 0,
                Some(1) => return 1,
                Some(_) => {
                    eprintln!("post plan could not serve post {id}: falling back to SQL");
                    self.plan_ok = false;
                }
                None => self.plan_ok = false,
            }
        }
        self.plan_close();
        self.post_sql(id, out)
    }

    pub fn create(&mut self, uid: i64, body: &[u8]) -> Option<(i64, Vec<u8>)> {
        if !self.ready() {
            return None;
        }
        self.plan_close();
        self.dirty_stmts |= STMT_INSERT;
        let mut result = None;
        unsafe {
            let bound = body.len() <= c_int::MAX as usize
                && sqlite3_bind_int64(self.q_insert, 1, uid) == SQLITE_OK
                && sqlite3_bind_text(
                    self.q_insert,
                    2,
                    body.as_ptr() as *const c_char,
                    body.len() as c_int,
                    None,
                ) == SQLITE_OK;
            if bound && sqlite3_step(self.q_insert) == SQLITE_ROW {
                let id = sqlite3_column_int64(self.q_insert, 0);
                let created = column(self.q_insert, 1).to_vec();
                // RETURNING's ROW does not establish statement completion.
                if sqlite3_step(self.q_insert) == SQLITE_DONE {
                    result = Some((id, created));
                }
            }
        }
        if self.finish_statement(STMT_INSERT, self.q_insert, result.is_some()) {
            result
        } else {
            None
        }
    }

    pub fn like(&mut self, uid: i64, post_id: i64) -> LikeResult {
        if !self.ready() {
            return LikeResult::Error;
        }
        self.plan_close();
        self.dirty_stmts |= STMT_EXISTS;
        let bound = unsafe { sqlite3_bind_int64(self.q_exists, 1, post_id) } == SQLITE_OK;
        let rc = if bound {
            unsafe { sqlite3_step(self.q_exists) }
        } else {
            -1
        };
        if !self.finish_statement(
            STMT_EXISTS,
            self.q_exists,
            rc == SQLITE_ROW || rc == SQLITE_DONE,
        ) {
            return LikeResult::Error;
        }
        if rc == SQLITE_DONE {
            return LikeResult::Missing;
        }
        self.dirty_stmts |= STMT_LIKE;
        let bound = unsafe {
            sqlite3_bind_int64(self.q_like, 1, uid) == SQLITE_OK
                && sqlite3_bind_int64(self.q_like, 2, post_id) == SQLITE_OK
        };
        let rc = if bound {
            unsafe { sqlite3_step(self.q_like) }
        } else {
            -1
        };
        let changes = unsafe { sqlite3_changes(self.h) };
        if !self.finish_statement(STMT_LIKE, self.q_like, rc == SQLITE_DONE) {
            return LikeResult::Error;
        }
        if changes == 1 {
            LikeResult::Created
        } else {
            LikeResult::AlreadyLiked
        }
    }

    pub fn batch_failed(&self) -> bool {
        self.batch_failed || self.broken
    }

    // Unrecoverable rollback/ownership failure must release locks immediately,
    // rather than retaining a failed transaction until a later socket event.
    fn retire_handle(&mut self) {
        self.broken = true;
        self.plan_ok = false;
        self.plan_close();
        drop(self.plan.take());
        if let Some(ckpt) = &self.checkpointer {
            ckpt.state.write_active.store(false, Ordering::Release);
            let _g = ckpt.state.mu.lock().unwrap();
            ckpt.state.cv.notify_all();
        }
        if !self.h.is_null() {
            unsafe {
                feed_install_wal_counter(self.h, std::ptr::null_mut());
            }
        }
        for slot in [
            &mut self.q_feed,
            &mut self.q_get,
            &mut self.q_exists,
            &mut self.q_insert,
            &mut self.q_like,
            &mut self.q_ping,
            &mut self.q_root,
            &mut self.q_begin,
            &mut self.q_begin_imm,
            &mut self.q_commit,
            &mut self.q_rollback,
        ] {
            if !slot.is_null() {
                unsafe {
                    sqlite3_finalize(*slot);
                }
                *slot = std::ptr::null_mut();
            }
        }
        self.dirty_stmts = 0;
        if !self.h.is_null() {
            // All owned cursors/statements are gone. close_v2 is a defensive
            // fallback if SQLite still reports an outstanding internal owner.
            let mut rc = unsafe { sqlite3_close(self.h) };
            if rc != SQLITE_OK {
                rc = unsafe { sqlite3_close_v2(self.h) };
            }
            if rc == SQLITE_OK {
                self.h = std::ptr::null_mut();
            } else {
                eprintln!("sqlite: handle retirement failed ({rc})");
            }
        }
    }

    pub fn enable_wal_checkpointer(&mut self, path: &str) {
        let ckpt_pages = std::env::var("FEED_WAL_CKPT_PAGES")
            .ok()
            .and_then(|s| s.parse::<u64>().ok())
            .filter(|&v| v > 0)
            .unwrap_or(DEFAULT_WAL_CKPT_PAGES);
        self.enable_wal_checkpointer_with_threshold(path, ckpt_pages);
    }

    pub fn enable_wal_checkpointer_with_threshold(&mut self, path: &str, ckpt_pages: u64) {
        if self.broken || self.h.is_null() || self.checkpointer.is_some() {
            return;
        }
        self.exec("PRAGMA wal_autocheckpoint = 0");
        self.checkpointer = WalCheckpointer::start(path, self.h, ckpt_pages);
    }

    #[inline]
    pub fn wal_restart_requested(&self) -> bool {
        self.checkpointer
            .as_ref()
            .map_or(false, |c| c.state.restart_requested.load(Ordering::Acquire))
    }

    pub fn wal_checkpoint_stats(&self) -> (u64, u64, u64, u64) {
        match &self.checkpointer {
            Some(c) => (
                c.state.wal_pages.load(Ordering::Acquire),
                c.state.passive_ok.load(Ordering::Relaxed),
                c.state.restart_ok.load(Ordering::Relaxed),
                c.state.restart_busy.load(Ordering::Relaxed),
            ),
            None => (0, 0, 0, 0),
        }
    }

    /// SQL owns every batch, including direct B-tree reads. Cursors open lazily
    /// on the first planned read in the batch so write-only or /health batches
    /// do not open and immediately close read cursors.
    pub fn batch_begin(&mut self, write: bool) {
        if self.in_batch {
            return;
        }
        self.batch_failed = false;
        if self.broken || !self.cleanup() {
            self.retire_handle();
            self.batch_failed = true;
            self.in_batch = true;
            return;
        }
        if write {
            if let Some(ckpt) = &self.checkpointer {
                ckpt.state.write_active.store(true, Ordering::Release);
            }
        }
        let (bit, stmt) = if write {
            (STMT_BEGIN_IMM, self.q_begin_imm)
        } else {
            (STMT_BEGIN, self.q_begin)
        };
        let rc = self.run_txn_stmt(bit, stmt);
        self.in_batch = true;
        if rc != SQLITE_OK || unsafe { sqlite3_get_autocommit(self.h) } != 0 {
            if let Some(ckpt) = &self.checkpointer {
                ckpt.state.write_active.store(false, Ordering::Release);
            }
            self.batch_failed = true;
            return;
        }
        self.ready();
    }

    /// No dependent output may be published until this returns true. A failed
    /// or lost transaction is never replayed, even if a reply is abandoned.
    pub fn batch_commit(&mut self) -> bool {
        if !self.in_batch {
            return !self.broken;
        }
        self.plan_close();
        let mut ok = self.ready();
        if ok {
            ok = self.run_txn_stmt(STMT_COMMIT, self.q_commit) == SQLITE_OK && self.idle();
        }
        if !self.cleanup() {
            self.retire_handle();
            ok = false;
        }
        self.in_batch = false;
        self.batch_failed = !ok;
        if let Some(ckpt) = &self.checkpointer {
            ckpt.state.write_active.store(false, Ordering::Release);
            let need_wake = ckpt.state.restart_requested.load(Ordering::Acquire)
                || ckpt.state.wal_pages.load(Ordering::Acquire) >= ckpt.ckpt_pages;
            if need_wake {
                let _g = ckpt.state.mu.lock().unwrap();
                ckpt.state.cv.notify_all();
            }
        }
        ok
    }
}

extern "C" {
    #[link_name = "feed_keyinfo"]
    fn feed_keyinfo_shim(db: *mut Sqlite3, nkey: c_int, nextra: c_int) -> *mut c_void;
    #[link_name = "feed_keyinfo_desc"]
    fn feed_keyinfo_desc_shim(db: *mut Sqlite3, nkey: c_int, nextra: c_int) -> *mut c_void;
    fn feed_keyinfo_free(p: *mut c_void);
}

unsafe fn feed_keyinfo_release(p: *mut c_void) {
    feed_keyinfo_free(p)
}

impl Drop for Db {
    fn drop(&mut self) {
        if !self.h.is_null() {
            unsafe {
                feed_install_wal_counter(self.h, std::ptr::null_mut());
            }
        }
        if let Some(mut ckpt) = self.checkpointer.take() {
            ckpt.stop();
        }
        if !self.cleanup() {
            eprintln!("sqlite: shutdown cleanup failed; retiring handle");
        }
        self.retire_handle();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    static SEQ: AtomicUsize = AtomicUsize::new(0);

    fn temp_db_path(label: &str) -> String {
        let n = SEQ.fetch_add(1, Ordering::Relaxed);
        format!("/tmp/native-l2-db-test-{}-{n}-{label}.db", std::process::id())
    }

    fn init_db(path: &str, sql_extra: &str) -> Db {
        for suffix in ["", "-wal", "-shm"] {
            let _ = std::fs::remove_file(format!("{path}{suffix}"));
        }
        let cp = CString::new(path).unwrap();
        let mut h: *mut Sqlite3 = std::ptr::null_mut();
        assert_eq!(
            unsafe { sqlite3_open_v2(cp.as_ptr(), &mut h, 6, std::ptr::null()) },
            SQLITE_OK
        );
        let schema = concat!(
            "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, ",
            "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));",
            "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), ",
            "body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500), ",
            "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));",
            "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), ",
            "post_id INTEGER NOT NULL REFERENCES posts(id), ",
            "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), ",
            "PRIMARY KEY (user_id, post_id));",
            "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);",
            "CREATE INDEX posts_user_id_idx ON posts (user_id);",
            "CREATE INDEX likes_post_id_idx ON likes (post_id);"
        );
        let cschema = CString::new(format!("{schema}{sql_extra}")).unwrap();
        assert_eq!(
            unsafe {
                sqlite3_exec(
                    h,
                    cschema.as_ptr(),
                    std::ptr::null(),
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                )
            },
            SQLITE_OK
        );
        assert_eq!(unsafe { sqlite3_close(h) }, SQLITE_OK);
        Db::open_with_cache(path, 128)
    }

    fn cleanup_files(path: &str) {
        for suffix in ["", "-wal", "-shm"] {
            let _ = std::fs::remove_file(format!("{path}{suffix}"));
        }
    }

    #[test]
    fn dirty_mask_tracks_and_clears_statements() {
        let path = temp_db_path("dirty");
        let mut db = init_db(
            &path,
            "INSERT INTO users(id,username) VALUES(1,'alice');\
             INSERT INTO posts(id,user_id,body,created_at) VALUES(1,1,'seed','2026-01-01T00:00:00.000Z');",
        );
        assert_eq!(db.dirty_stmts, 0);

        // Planned read batch uses B-tree cursors and transaction statements only.
        db.batch_begin(false);
        assert_eq!(db.dirty_stmts, 0);
        let mut out = Vec::new();
        assert!(db.feed_json(&mut out));
        assert_eq!(db.dirty_stmts, 0);
        assert!(db.batch_commit());
        assert_eq!(db.dirty_stmts, 0);

        // Statement CHECK abort + write leaves bindings cleared and resets cleanly on commit.
        db.batch_begin(true);
        assert!(db.create(1, b"").is_none());
        assert!(db.create(1, b"ok").is_some());
        assert_eq!(db.dirty_stmts, 0);

        // Mark a bound statement dirty on purpose and confirm cleanup clears it.
        assert_eq!(unsafe { sqlite3_bind_int64(db.q_get, 1, 1) }, SQLITE_OK);
        db.dirty_stmts |= STMT_GET;
        assert!(db.batch_commit());
        assert_eq!(db.dirty_stmts, 0);

        drop(db);
        cleanup_files(&path);
    }

    #[test]
    fn btree_256_span_and_fallback_match_sql_with_gaps_and_reordering() {
        let path = temp_db_path("span");
        // Posts span exactly 256 (ids 2..=258) with huge gaps and inverted created_at order.
        let mut db = init_db(
            &path,
            "INSERT INTO users(id,username) VALUES(1,'alice'),(2,'bob'),(3,'carol');\
             INSERT INTO posts(id,user_id,body,created_at) VALUES\
               (2,   1, 'oldest-id-newest-ts', '2026-06-01T00:00:00.000Z'),\
               (15,  2, 'mid-low',             '2026-03-01T00:00:00.000Z'),\
               (199, 3, 'mid-high',            '2026-05-01T00:00:00.000Z'),\
               (258, 1, 'high-id-older-ts',    '2026-02-01T00:00:00.000Z');\
             INSERT INTO likes(user_id,post_id) VALUES\
               (1,2),(2,2),(3,2),(1,199),(2,258);",
        );
        assert!(db.plan_ok);

        db.batch_begin(false);
        let mut planned = Vec::new();
        let mut reference = Vec::new();
        assert!(db.feed_json(&mut planned));
        assert!(db.feed_sql(&mut reference));
        assert_eq!(planned, reference);
        assert!(db.batch_commit());

        // Push span to 257 (ids 2..=259) to exercise the per-post >256 fallback path.
        db.exec("INSERT INTO posts(id,user_id,body,created_at) VALUES(259, 2, 'span-257', '2026-04-01T00:00:00.000Z')");
        db.exec("INSERT INTO likes(user_id,post_id) VALUES(1,259),(3,259)");

        db.batch_begin(false);
        planned.clear();
        reference.clear();
        assert!(db.feed_json(&mut planned));
        assert!(db.feed_sql(&mut reference));
        assert_eq!(planned, reference);
        assert!(db.batch_commit());

        drop(db);
        cleanup_files(&path);
    }

    #[test]
    fn wal_checkpointer_bounds_pages_and_drops_cleanly() {
        let path = temp_db_path("walckpt");
        let mut db = init_db(&path, "INSERT INTO users(id,username) VALUES(1,'alice');");
        db.enable_wal_checkpointer_with_threshold(&path, 4);

        // Hold an external reader snapshot so initial RESTART encounters SQLITE_BUSY
        // and backs off cleanly without blocking writers.
        let cp = CString::new(path.clone()).unwrap();
        let mut ext_h: *mut Sqlite3 = std::ptr::null_mut();
        assert_eq!(
            unsafe { sqlite3_open_v2(cp.as_ptr(), &mut ext_h, SQLITE_OPEN_READWRITE, std::ptr::null()) },
            SQLITE_OK
        );
        let begin_sql = CString::new("BEGIN; SELECT count(*) FROM users;").unwrap();
        assert_eq!(
            unsafe {
                sqlite3_exec(
                    ext_h,
                    begin_sql.as_ptr(),
                    std::ptr::null(),
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                )
            },
            SQLITE_OK
        );

        let body = vec![b'w'; 450];
        for _ in 0..20 {
            db.batch_begin(true);
            assert!(db.create(1, &body).is_some());
            assert!(db.batch_commit());
        }

        // Wait up to 1.5s for at least one PASSIVE checkpoint & BUSY restart attempt.
        let deadline = Instant::now() + Duration::from_millis(1500);
        while Instant::now() < deadline {
            let (_, p_ok, _, r_busy) = db.wal_checkpoint_stats();
            if p_ok >= 1 && r_busy >= 1 {
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        let (_, p_ok, _, _) = db.wal_checkpoint_stats();
        assert!(p_ok >= 1, "expected PASSIVE checkpoint under WAL pressure");

        // Release external reader and confirm RESTART succeeds cleanly after cooldown.
        let end_sql = CString::new("COMMIT;").unwrap();
        assert_eq!(
            unsafe {
                sqlite3_exec(
                    ext_h,
                    end_sql.as_ptr(),
                    std::ptr::null(),
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                )
            },
            SQLITE_OK
        );
        assert_eq!(unsafe { sqlite3_close(ext_h) }, SQLITE_OK);

        let deadline = Instant::now() + Duration::from_millis(2800);
        while Instant::now() < deadline {
            if db.wal_checkpoint_stats().2 >= 1 {
                break;
            }
            db.batch_begin(true);
            assert!(db.create(1, &body).is_some());
            assert!(db.batch_commit());
            std::thread::sleep(Duration::from_millis(100));
        }
        assert!(db.wal_checkpoint_stats().2 >= 1, "expected RESTART checkpoint after reader closed");
        assert!(!db.wal_restart_requested());

        drop(db);
        cleanup_files(&path);
    }
}
