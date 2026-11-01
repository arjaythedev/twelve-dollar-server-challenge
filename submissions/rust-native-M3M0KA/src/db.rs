//! SQLite access through its C API, plus the optional B-tree read plans.
//!
//! One connection, one thread, prepared statements created once. Every response
//! is serialized from rows read while its request is being served; nothing is
//! cached across requests. WAL + `synchronous=NORMAL` keeps a commit durable
//! before the response that reports it is allowed to leave.

use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};
use std::sync::Arc;

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
const SQLITE_OPEN_READWRITE: c_int = 0x0000_0002;
const SQLITE_PREPARE_PERSISTENT: u32 = 0x08;

extern "C" {
    fn sqlite3_open_v2(
        filename: *const c_char,
        db: *mut *mut Sqlite3,
        flags: c_int,
        vfs: *const c_char,
    ) -> c_int;
    fn sqlite3_close(db: *mut Sqlite3) -> c_int;
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
    fn sqlite3_column_int64(s: *mut Stmt, i: c_int) -> i64;
    fn sqlite3_column_text(s: *mut Stmt, i: c_int) -> *const c_char;
    fn sqlite3_column_bytes(s: *mut Stmt, i: c_int) -> c_int;
    fn sqlite3_changes(db: *mut Sqlite3) -> c_int;
    fn setpriority(which: c_int, who: c_int, nice: c_int) -> c_int;
    fn sqlite3_wal_hook(
        db: *mut Sqlite3,
        cb: Option<unsafe extern "C" fn(*mut c_void, *mut Sqlite3, *const c_char, c_int) -> c_int>,
        ud: *mut c_void,
    ) -> Option<
        unsafe extern "C" fn(*mut c_void, *mut Sqlite3, *const c_char, c_int) -> c_int,
    >;
    fn sqlite3_wal_checkpoint_v2(
        db: *mut Sqlite3,
        zdb: *const c_char,
        mode: c_int,
        log: *mut c_int,
        ckpt: *mut c_int,
    ) -> c_int;
    fn syscall(num: c_int, ...) -> c_int;
}

pub enum LikeResult {
    Created,
    AlreadyLiked,
    Missing,
    Error,
}

/// Off-loop WAL checkpointing, the shape the top boards run: the loop pays
/// nothing for checkpoints, and a dedicated thread at nice 19 copies the WAL
/// between the loop's batches. `wal_autocheckpoint` stays 0, so nothing inside
/// a serving statement ever blocks on a checkpoint copy.
///
/// - the WAL hook records the frame count and wakes the thread past
///   `wake_frames` (default 1,000 = ~4 MB of WAL);
/// - the thread runs one PASSIVE checkpoint on its own second connection and
///   stops when `nlog == nckpt` (everything backfilled). While the gate is
///   held — `gate_frames` exceeded — it keeps re-arming until the copy is
///   done, because with a still-open write the next commit would stall
///   otherwise;
/// - the thread is at nice 19: on one vCPU a pass at the default priority can
///   take the CPU for its whole length, and the measured effect at the edge
///   was holds dying at nice 0 and passing at 19. The loop's batches are
///   short, so the thread runs between them.
const CKPT_PASSIVE: c_int = 1; // SQLITE_CHECKPOINT_PASSIVE
const WAL_WAKE_FRAMES: i32 = 1000;
const WAL_GATE_FRAMES: i32 = 4096;

struct WalCtl {
    mu: std::sync::Mutex<bool>, // `wanted`
    cv: std::sync::Condvar,
    commit_seq: std::sync::atomic::AtomicU64,
    backfilled_seq: std::sync::atomic::AtomicU64,
    stop: std::sync::atomic::AtomicBool,
    gate_held: std::sync::atomic::AtomicBool,
    c2: *mut Sqlite3,
}

unsafe impl Send for WalCtl {}
unsafe impl Sync for WalCtl {}

impl WalCtl {
    fn new(c2: *mut Sqlite3) -> std::sync::Arc<WalCtl> {
        std::sync::Arc::new(WalCtl {
            mu: std::sync::Mutex::new(false),
            cv: std::sync::Condvar::new(),
            commit_seq: std::sync::atomic::AtomicU64::new(0),
            backfilled_seq: std::sync::atomic::AtomicU64::new(0),
            stop: std::sync::atomic::AtomicBool::new(false),
            gate_held: std::sync::atomic::AtomicBool::new(false),
            c2,
        })
    }

    /// One PASSIVE checkpoint on the second connection, then either done
    /// (everything backfilled) or re-armed while the gate is held.
    fn one_ckpt(&self) {
        use std::sync::atomic::Ordering::*;
        let seq = self.commit_seq.load(Acquire);
        let mut nlog: c_int = 0;
        let mut nckpt: c_int = 0;
        let name = b"main\0";
        let rc = unsafe {
            sqlite3_wal_checkpoint_v2(self.c2, name.as_ptr() as *const i8, CKPT_PASSIVE, &mut nlog, &mut nckpt)
        };
        let done = (rc == 0 || rc == 3) && nlog == nckpt;
        if done {
            self.backfilled_seq.store(seq, Release);
            self.gate_held.store(false, Release);
        }
        if !done && self.gate_held.load(Acquire) {
            let mut wanted = self.mu.lock().unwrap();
            *wanted = true;
            self.cv.notify_one();
        }
    }

    /// Owns `c2` for its whole lifetime. Runs at nice 19 so a checkpoint copy
    /// never preempts a loop pass; between passes the loop is out of the way
    /// for tens of microseconds, which is all this needs.
    fn run(self: std::sync::Arc<WalCtl>) {
        unsafe {
            setpriority(0, syscall(186), 19); // PRIO_PROCESS, gettid, nice
        }
        let mut wanted = self.mu.lock().unwrap();
        loop {
            let (guard, _) = self
                .cv
                .wait_timeout(wanted, std::time::Duration::from_millis(250))
                .unwrap();
            wanted = guard;
            if self.stop.load(std::sync::atomic::Ordering::Acquire) {
                return;
            }
            if *wanted {
                *wanted = false;
                std::mem::drop(wanted);
                self.one_ckpt();
                wanted = self.mu.lock().unwrap();
            }
        }
    }
}

unsafe extern "C" fn wal_hook(
    ud: *mut c_void,
    _db: *mut Sqlite3,
    _name: *const c_char,
    frames: c_int,
) -> c_int {
    let ctl = ud as *const WalCtl;
    let ctl = &*ctl;
    use std::sync::atomic::Ordering::*;
    ctl.commit_seq.fetch_add(1, Release);
    if frames >= WAL_WAKE_FRAMES {
        let mut wanted = ctl.mu.lock().unwrap();
        *wanted = true;
        ctl.cv.notify_one();
    }
    0
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

fn row_json(s: *mut Stmt, out: &mut Vec<u8>) {
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

pub struct Db {
    h: *mut Sqlite3,
    q_feed: *mut Stmt,
    q_get: *mut Stmt,
    q_exists: *mut Stmt,
    q_insert: *mut Stmt,
    q_like: *mut Stmt,
    q_ping: *mut Stmt,
    q_root: *mut Stmt,
    in_batch: bool,
    batch_failed: bool,
    plan: Option<Cursors>,
    plan_ok: bool,
    /// Off-loop WAL checkpoint control; None keeps the inline auto-checkpoint.
    wal: Option<std::sync::Arc<WalCtl>>,
}

impl Db {
    pub fn open_with_cache(path: &str, cache_kb: i32) -> Db {
        let cpath = CString::new(path).expect("path");
        let mut h: *mut Sqlite3 = std::ptr::null_mut();
        unsafe {
            if sqlite3_open_v2(cpath.as_ptr(), &mut h, SQLITE_OPEN_READWRITE, std::ptr::null())
                != SQLITE_OK
            {
                panic!("cannot open {path}");
            }
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
            in_batch: false,
            batch_failed: false,
            plan: None,
            plan_ok: false,
            wal: None,
        };
        // A PASSIVE checkpoint copies the accumulated frames into the database
        // file inside the statement that crosses the threshold. Smaller means a
        // shorter stall but more often; the total bytes copied are the same.
        let ac: i64 = std::env::var("FEED_WAL_AC")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(0);
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
        if ac == 0 {
            // The inline auto-checkpoint is off: a WAL hook counts frames and a
            // dedicated nice-19 thread copies the WAL between loop batches.
            let cpath = CString::new(path).expect("path");
            let mut c2: *mut Sqlite3 = std::ptr::null_mut();
            if unsafe {
                sqlite3_open_v2(cpath.as_ptr(), &mut c2, SQLITE_OPEN_READWRITE, std::ptr::null())
            } == SQLITE_OK
            {
                unsafe {
                    sqlite3_busy_timeout(c2, 5000);
                    db.exec("PRAGMA cache_size = -2048");
                }
                let ctl = WalCtl::new(c2);
                unsafe {
                    sqlite3_wal_hook(db.h, Some(wal_hook), Arc::as_ptr(&ctl) as *mut c_void);
                }
                let clone = Arc::clone(&ctl);
                std::thread::spawn(move || clone.run());
                db.wal = Some(ctl);
            }
        }
        const COLS: &str = "SELECT p.id, p.body, p.created_at, u.username, \
             (SELECT count(*) FROM likes l WHERE l.post_id = p.id) \
             FROM posts p JOIN users u ON u.id = p.user_id";
        db.q_feed = db.prepare(&format!(
            "{COLS} ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
        ));
        db.q_get = db.prepare(&format!("{COLS} WHERE p.id = ?1"));
        db.q_exists = db.prepare("SELECT 1 FROM posts WHERE id = ?1");
        db.q_insert = db.prepare(
            "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at",
        );
        db.q_like = db.prepare(
            "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) \
             ON CONFLICT (user_id, post_id) DO NOTHING",
        );
        db.q_ping = db.prepare("SELECT 1");
        db.q_root = db.prepare("SELECT rootpage FROM sqlite_schema WHERE name = ?1");
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
                SQLITE_PREPARE_PERSISTENT,
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

    fn root(&self, name: &str) -> Option<u32> {
        unsafe {
            sqlite3_bind_text(self.q_root, 1, name.as_ptr() as *const c_char, name.len() as c_int, None);
            let rc = sqlite3_step(self.q_root);
            let v = if rc == SQLITE_ROW {
                sqlite3_column_int64(self.q_root, 0) as u32
            } else {
                0
            };
            sqlite3_reset(self.q_root);
            if v == 0 {
                None
            } else {
                Some(v)
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
                out.push(if sqlite3_column_int64(s, 3) != 0 { '1' } else { '0' });
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
            return;
        }
        let mut c = Cursors::new(self.h as *mut c_void, (rp, ru, rl, rf), ki, ki_newest);
        if c.open().is_none() {
            eprintln!("cursor setup failed: read plans disabled");
            unsafe { feed_keyinfo_release(ki) };
            return;
        }
        let mut planned = Vec::new();
        let planned_res = c.feed_json(&mut planned);
        let planned_ok = planned_res.is_ok();
        c.close();

        let mut reference = Vec::new();
        let sql_ok = self.feed_sql(&mut reference);
        let agree = if planned_ok {
            if sql_ok && planned != reference {
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
            sql_ok && planned == reference
        } else {
            eprintln!("feed plan self check failed: {planned_res:?}");
            false
        };
        if !agree {
            eprintln!("feed plan disagrees with SQL: read plans disabled");
            unsafe { feed_keyinfo_release(ki) };
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

    pub fn ping(&mut self) -> bool {
        unsafe {
            let rc = sqlite3_step(self.q_ping);
            sqlite3_reset(self.q_ping);
            rc == SQLITE_ROW
        }
    }

    fn feed_sql(&mut self, out: &mut Vec<u8>) -> bool {
        out.extend_from_slice(b"{\"posts\":[");
        let mut first = true;
        loop {
            let rc = unsafe { sqlite3_step(self.q_feed) };
            if rc == SQLITE_ROW {
                if !first {
                    out.push(b',');
                }
                first = false;
                row_json(self.q_feed, out);
                continue;
            }
            unsafe { sqlite3_reset(self.q_feed) };
            if rc != SQLITE_DONE {
                return false;
            }
            break;
        }
        out.extend_from_slice(b"]}");
        true
    }

    pub fn feed_json(&mut self, out: &mut Vec<u8>) -> bool {
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
        self.feed_sql(out)
    }

    fn post_sql(&mut self, id: i64, out: &mut Vec<u8>) -> i32 {
        unsafe {
            sqlite3_bind_int64(self.q_get, 1, id);
            let rc = sqlite3_step(self.q_get);
            if rc == SQLITE_ROW {
                out.extend_from_slice(b"{\"post\":");
                row_json(self.q_get, out);
                out.push(b'}');
                sqlite3_reset(self.q_get);
                return 0;
            }
            sqlite3_reset(self.q_get);
            if rc == SQLITE_DONE {
                1
            } else {
                -1
            }
        }
    }

    pub fn post_json(&mut self, id: i64, out: &mut Vec<u8>) -> i32 {
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
        self.post_sql(id, out)
    }

    pub fn create(&mut self, uid: i64, body: &[u8]) -> Option<(i64, Vec<u8>)> {
        // The read cursors must not be positioned while SQLite modifies pages.
        self.plan_close();
        unsafe {
            sqlite3_bind_int64(self.q_insert, 1, uid);
            sqlite3_bind_text(
                self.q_insert,
                2,
                body.as_ptr() as *const c_char,
                body.len() as c_int,
                None,
            );
            if sqlite3_step(self.q_insert) != SQLITE_ROW {
                sqlite3_reset(self.q_insert);
                return None;
            }
            let id = sqlite3_column_int64(self.q_insert, 0);
            let created = column(self.q_insert, 1).to_vec();
            sqlite3_reset(self.q_insert);
            Some((id, created))
        }
    }

    pub fn like(&mut self, uid: i64, post_id: i64) -> LikeResult {
        self.plan_close();
        unsafe {
            sqlite3_bind_int64(self.q_exists, 1, post_id);
            let rc = sqlite3_step(self.q_exists);
            sqlite3_reset(self.q_exists);
            if rc == SQLITE_DONE {
                return LikeResult::Missing;
            }
            if rc != SQLITE_ROW {
                return LikeResult::Error;
            }

            sqlite3_bind_int64(self.q_like, 1, uid);
            sqlite3_bind_int64(self.q_like, 2, post_id);
            let rc = sqlite3_step(self.q_like);
            sqlite3_reset(self.q_like);
            if rc != SQLITE_DONE {
                return LikeResult::Error;
            }
            if sqlite3_changes(self.h) == 1 {
                LikeResult::Created
            } else {
                LikeResult::AlreadyLiked
            }
        }
    }

    pub fn batch_failed(&self) -> bool {
        self.batch_failed
    }

    /// Opens the transaction that covers one event-loop batch.
    /// `write` takes the write lock up front. A pass that writes after taking a
    /// read snapshot would get SQLITE_BUSY at once when another process has
    /// committed meanwhile; busy_timeout cannot help there.
    pub fn batch_begin(&mut self, write: bool) {
        if self.in_batch {
            return;
        }
        let sql = if write { "BEGIN IMMEDIATE" } else { "BEGIN" };
        let csql = CString::new(sql).unwrap();
        let rc = unsafe {
            sqlite3_exec(self.h, csql.as_ptr(), std::ptr::null(), std::ptr::null_mut(), std::ptr::null_mut())
        };
        if rc != SQLITE_OK {
            // Nothing in this pass may run outside a transaction, so the pass
            // is answered 503 and not executed at all.
            self.batch_failed = true;
            self.in_batch = true;
            return;
        }
        self.batch_failed = false;
        self.in_batch = true;
        // Every read in this transaction starts from SQLite's stored pages, so
        // nothing here depends on what another connection did since the last
        // pass: a write by any process is simply read, not tracked.
        self.plan_reopen();
    }

    /// Commits the batch. Returns false (and rolls back) when the commit fails.
    pub fn batch_commit(&mut self) -> bool {
        if !self.in_batch {
            return true;
        }
        if self.batch_failed {
            self.batch_failed = false;
            self.in_batch = false;
            return false;
        }
        self.plan_close();
        self.in_batch = false;
        let csql = CString::new("COMMIT").unwrap();
        unsafe {
            if sqlite3_exec(
                self.h,
                csql.as_ptr(),
                std::ptr::null(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            ) == SQLITE_OK
            {
                return true;
            }
            let rb = CString::new("ROLLBACK").unwrap();
            sqlite3_exec(
                self.h,
                rb.as_ptr(),
                std::ptr::null(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            );
        }
        false
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
        unsafe {
            for s in [
                self.q_feed,
                self.q_get,
                self.q_exists,
                self.q_insert,
                self.q_like,
                self.q_ping,
                self.q_root,
            ] {
                if !s.is_null() {
                    sqlite3_finalize(s);
                }
            }
            if !self.h.is_null() {
                sqlite3_close(self.h);
            }
        }
    }
}
