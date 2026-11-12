use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::ptr;


#[repr(C)]
pub struct Sqlite3 {
    _private: [u8; 0],
}

#[repr(C)]
pub struct Sqlite3Stmt {
    _private: [u8; 0],
}

#[repr(C)]
pub struct BtreeReader {
    _private: [u8; 0],
}

const SQLITE_OK: c_int = 0;
const SQLITE_ROW: c_int = 100;
const SQLITE_DONE: c_int = 101;
const SQLITE_OPEN_READWRITE: c_int = 0x0000_0002;
const SQLITE_OPEN_CREATE: c_int = 0x0000_0004;
const SQLITE_OPEN_NOMUTEX: c_int = 0x0000_8000;

extern "C" {
    fn sqlite3_open_v2(
        filename: *const c_char,
        pp_db: *mut *mut Sqlite3,
        flags: c_int,
        vfs: *const c_char,
    ) -> c_int;
    fn sqlite3_close(db: *mut Sqlite3) -> c_int;
    fn sqlite3_errmsg(db: *mut Sqlite3) -> *const c_char;
    fn sqlite3_busy_timeout(db: *mut Sqlite3, ms: c_int) -> c_int;
    fn sqlite3_exec(
        db: *mut Sqlite3,
        sql: *const c_char,
        cb: Option<unsafe extern "C" fn(*mut c_void, c_int, *mut *mut c_char, *mut *mut c_char) -> c_int>,
        arg: *mut c_void,
        err: *mut *mut c_char,
    ) -> c_int;
    fn sqlite3_prepare_v2(
        db: *mut Sqlite3,
        sql: *const c_char,
        nbyte: c_int,
        stmt: *mut *mut Sqlite3Stmt,
        tail: *mut *const c_char,
    ) -> c_int;
    fn sqlite3_finalize(stmt: *mut Sqlite3Stmt) -> c_int;
    fn sqlite3_reset(stmt: *mut Sqlite3Stmt) -> c_int;
    fn sqlite3_bind_int64(stmt: *mut Sqlite3Stmt, idx: c_int, value: i64) -> c_int;
    fn sqlite3_bind_text(
        stmt: *mut Sqlite3Stmt,
        idx: c_int,
        value: *const c_char,
        n: c_int,
        destructor: Option<unsafe extern "C" fn(*mut c_void)>,
    ) -> c_int;
    fn sqlite3_step(stmt: *mut Sqlite3Stmt) -> c_int;
    fn sqlite3_column_int64(stmt: *mut Sqlite3Stmt, col: c_int) -> i64;
    fn sqlite3_column_text(stmt: *mut Sqlite3Stmt, col: c_int) -> *const u8;
    fn sqlite3_column_bytes(stmt: *mut Sqlite3Stmt, col: c_int) -> c_int;
    fn sqlite3_changes(db: *mut Sqlite3) -> c_int;

    fn btree_reader_free(reader: *mut BtreeReader);
    fn btree_reader_begin(reader: *mut BtreeReader) -> c_int;
    fn btree_reader_end(reader: *mut BtreeReader, ok: c_int) -> c_int;
    fn btree_reader_reopen(reader: *mut BtreeReader) -> c_int;
    fn btree_reader_close_cursors(reader: *mut BtreeReader);
    pub(crate) fn btree_stats_enable(on: c_int);
    pub(crate) fn btree_stats(out: *mut u64);
    fn btree_reader_new(
        db: *mut Sqlite3,
        posts_root: u32,
        users_root: u32,
        posts_idx_root: u32,
        likes_idx_root: u32,
    ) -> *mut BtreeReader;
    fn btree_reader_refresh_feed(reader: *mut BtreeReader);
    fn btree_reader_refresh_post(reader: *mut BtreeReader);
    fn btree_reader_refresh_like(reader: *mut BtreeReader);
    fn feed_btree(reader: *mut BtreeReader, out: *mut c_char, cap: c_int, out_len: *mut c_int) -> c_int;
    fn feed_btree_tx(reader: *mut BtreeReader, out: *mut c_char, cap: c_int, out_len: *mut c_int) -> c_int;
    fn posts_btree(
        reader: *mut BtreeReader,
        ids: *const i64,
        n: c_int,
        out: *mut c_char,
        cap: c_int,
        out_len: *mut c_int,
        lens: *mut c_int,
    ) -> c_int;
    fn posts_btree_tx(
        reader: *mut BtreeReader,
        ids: *const i64,
        n: c_int,
        out: *mut c_char,
        cap: c_int,
        out_len: *mut c_int,
        lens: *mut c_int,
    ) -> c_int;
}

#[inline]
fn errmsg(db: *mut Sqlite3) -> String {
    unsafe {
        if db.is_null() {
            return "sqlite error".to_string();
        }
        let p = sqlite3_errmsg(db);
        if p.is_null() {
            "sqlite error".to_string()
        } else {
            CStr::from_ptr(p).to_string_lossy().into_owned()
        }
    }
}

pub struct Db {
    raw: *mut Sqlite3,
}

impl Db {
    pub fn open(path: &str) -> Result<Self, String> {
        let cpath = CString::new(path).map_err(|_| "bad sqlite path".to_string())?;
        let mut raw = ptr::null_mut();
        let rc = unsafe {
            sqlite3_open_v2(
                cpath.as_ptr(),
                &mut raw,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                ptr::null(),
            )
        };
        if rc != SQLITE_OK || raw.is_null() {
            let msg = errmsg(raw);
            if !raw.is_null() {
                unsafe {
                    sqlite3_close(raw);
                }
            }
            return Err(msg);
        }
        unsafe {
            sqlite3_busy_timeout(raw, 5000);
        }
        let db = Self { raw };
        db.exec("PRAGMA journal_mode=WAL")?;
        db.exec("PRAGMA synchronous=NORMAL")?;
        let ckpt = std::env::var("WAL_AUTOCHECKPOINT")
            .ok()
            .and_then(|v| v.parse::<i64>().ok())
            .unwrap_or(10_000)
            .max(0);
        db.exec(&format!("PRAGMA wal_autocheckpoint={}", ckpt))?;
        db.exec("PRAGMA cache_size=-16384")?;
        Ok(db)
    }

    pub fn raw(&self) -> *mut Sqlite3 {
        self.raw
    }

    pub fn exec(&self, sql: &str) -> Result<(), String> {
        let c = CString::new(sql).map_err(|_| "bad sql".to_string())?;
        let rc = unsafe { sqlite3_exec(self.raw, c.as_ptr(), None, ptr::null_mut(), ptr::null_mut()) };
        if rc == SQLITE_OK {
            Ok(())
        } else {
            Err(errmsg(self.raw))
        }
    }

    pub fn prepare(&self, sql: &str) -> Result<Stmt<'_>, String> {
        let c = CString::new(sql).map_err(|_| "bad sql".to_string())?;
        let mut stmt = ptr::null_mut();
        let rc = unsafe {
            sqlite3_prepare_v2(self.raw, c.as_ptr(), -1, &mut stmt, ptr::null_mut())
        };
        if rc != SQLITE_OK || stmt.is_null() {
            return Err(errmsg(self.raw));
        }
        Ok(Stmt { db: self.raw, raw: stmt, _marker: std::marker::PhantomData })
    }
}

impl Drop for Db {
    fn drop(&mut self) {
        unsafe {
            sqlite3_close(self.raw);
        }
    }
}

pub struct Stmt<'a> {
    db: *mut Sqlite3,
    raw: *mut Sqlite3Stmt,
    _marker: std::marker::PhantomData<&'a Db>,
}

impl<'a> Stmt<'a> {
    pub fn bind_i64(&mut self, idx: i32, value: i64) {
        unsafe {
            sqlite3_bind_int64(self.raw, idx, value);
        }
    }

    pub fn bind_text(&mut self, idx: i32, value: &str) {
        unsafe {
            sqlite3_bind_text(
                self.raw,
                idx,
                value.as_ptr() as *const c_char,
                value.len() as c_int,
                None,
            );
        }
    }

    pub fn step(&mut self) -> Result<bool, String> {
        let rc = unsafe { sqlite3_step(self.raw) };
        match rc {
            SQLITE_ROW => Ok(true),
            SQLITE_DONE => Ok(false),
            _ => Err(errmsg(self.db)),
        }
    }

    pub fn reset(&mut self) {
        unsafe {
            sqlite3_reset(self.raw);
        }
    }

    pub fn column_i64(&self, col: i32) -> i64 {
        unsafe { sqlite3_column_int64(self.raw, col) }
    }

    pub fn column_text(&self, col: i32) -> String {
        unsafe {
            let p = sqlite3_column_text(self.raw, col);
            if p.is_null() {
                return String::new();
            }
            let n = sqlite3_column_bytes(self.raw, col) as usize;
            String::from_utf8_lossy(std::slice::from_raw_parts(p, n)).into_owned()
        }
    }
}

impl Drop for Stmt<'_> {
    fn drop(&mut self) {
        unsafe {
            sqlite3_reset(self.raw);
            sqlite3_finalize(self.raw);
        }
    }
}

#[inline]
fn changes(db: *mut Sqlite3) -> i32 {
    unsafe { sqlite3_changes(db) }
}

pub struct ReadDb {
    db: Db,
    path: String,
    roots: Option<(u32, u32, u32, u32)>,
    needs_feed: bool,
    needs_post: bool,
    needs_like: bool,
    reader: *mut BtreeReader,
    in_tx: bool,
    txn_stale: bool,
    /// Scratch used only while a call is being served.
    pub feed_out: Vec<u8>,
    pub post_out: Vec<u8>,
}

impl Drop for ReadDb {
    fn drop(&mut self) {
        unsafe {
            if !self.reader.is_null() {
                btree_reader_free(self.reader);
            }
        }
    }
}

impl ReadDb {
    pub fn open(path: &str) -> Result<Self, String> {
        let db = Db::open(path)?;
        db.exec("PRAGMA busy_timeout=200")?;
        let cache_mb = std::env::var("READ_CACHE_MB")
            .ok()
            .and_then(|v| v.parse::<i64>().ok())
            .unwrap_or(64)
            .max(1);
        // The B-tree shim serves payloads as pointers into the pager's page
        // cache, so a mapping only removes work on a cache miss, where the
        // pager would otherwise pread into the cache page. Half the seed
        // database bounds the mapped RSS (~128 MB) while covering the hot
        // working set. mmap is explicitly allowed by rule 5.
        let mmap_mb = std::env::var("READ_MMAP_MB")
            .ok()
            .and_then(|v| v.parse::<i64>().ok())
            .unwrap_or(128)
            .max(0);
        db.exec(&format!("PRAGMA cache_size=-{}", cache_mb * 1024))?;
        db.exec(&format!("PRAGMA mmap_size={}", mmap_mb * 1024 * 1024))?;
        let roots = load_roots(&db);
        let reader = match roots {
            Some((posts, users, posts_idx, likes_idx)) => unsafe {
                btree_reader_new(db.raw(), posts, users, posts_idx, likes_idx)
            },
            None => ptr::null_mut(),
        };
        Ok(Self {
            db,
            path: path.to_string(),
            roots,
            needs_feed: false,
            needs_post: false,
            needs_like: false,
            reader,
            in_tx: false,
            txn_stale: false,
            feed_out: vec![0u8; 64 * 1024],
            post_out: vec![0u8; 128 * 1024],
        })
    }

    pub fn begin_read(&mut self) -> bool {
        if self.reader.is_null() {
            return true;
        }
        if self.in_tx {
            if !self.txn_stale {
                return true;
            }
            self.end_read(true);
        }
        if unsafe { btree_reader_begin(self.reader) } != SQLITE_OK {
            return false;
        }
        if unsafe { btree_reader_reopen(self.reader) } != SQLITE_OK {
            unsafe { btree_reader_end(self.reader, 0) };
            return false;
        }
        self.txn_stale = false;
        self.in_tx = true;
        true
    }

    pub fn end_read(&mut self, ok: bool) {
        if self.in_tx {
            unsafe {
                btree_reader_close_cursors(self.reader);
                btree_reader_end(self.reader, if ok { 1 } else { 0 });
            }
            self.in_tx = false;
        }
    }

    pub fn invalidate_feed(&mut self) {
        self.needs_feed = true;
        self.txn_stale = true;
    }

    pub fn invalidate_post(&mut self) {
        self.needs_post = true;
        self.txn_stale = true;
    }

    pub fn invalidate_like(&mut self) {
        self.needs_like = true;
        self.txn_stale = true;
    }

    pub fn refresh(&mut self) {
        if self.reader.is_null() {
            return;
        }
        unsafe {
            if self.needs_feed {
                btree_reader_refresh_feed(self.reader);
                self.needs_feed = false;
            }
            if self.needs_post {
                btree_reader_refresh_post(self.reader);
                self.needs_post = false;
            }
            if self.needs_like {
                btree_reader_refresh_like(self.reader);
                self.needs_like = false;
            }
        }

    }

    pub fn health(&self) -> bool {
        let Ok(mut st) = self.db.prepare("SELECT 1") else {
            return false;
        };
        matches!(st.step(), Ok(true))
    }

    /// Returns the complete response body.
    pub fn feed(&mut self) -> Result<Vec<u8>, ()> {
        if !self.reader.is_null() {
            let mut n = 0;
            let rc = unsafe {
                if self.in_tx {
                    feed_btree_tx(
                        self.reader,
                        self.feed_out.as_mut_ptr() as *mut c_char,
                        self.feed_out.len() as c_int,
                        &mut n,
                    )
                } else {
                    feed_btree(
                        self.reader,
                        self.feed_out.as_mut_ptr() as *mut c_char,
                        self.feed_out.len() as c_int,
                        &mut n,
                    )
                }
            };
            if rc == SQLITE_OK && n >= 0 {
                let array = &self.feed_out[..n as usize];
                let mut body = Vec::with_capacity(array.len() + 10);
                body.extend_from_slice(b"{\"posts\":");
                body.extend_from_slice(array);
                body.push(b'}');
                return Ok(body);
            }
        }
        self.feed_sql()
    }

    /// Complete HTTP response, avoiding a separate body allocation and copy.
    pub fn feed_response(&mut self) -> Result<Vec<u8>, ()> {
        if !self.reader.is_null() {
            let mut n = 0;
            let rc = unsafe {
                if self.in_tx {
                    feed_btree_tx(
                        self.reader,
                        self.feed_out.as_mut_ptr() as *mut c_char,
                        self.feed_out.len() as c_int,
                        &mut n,
                    )
                } else {
                    feed_btree(
                        self.reader,
                        self.feed_out.as_mut_ptr() as *mut c_char,
                        self.feed_out.len() as c_int,
                        &mut n,
                    )
                }
            };
            if rc == SQLITE_OK && n >= 0 {
                let array = &self.feed_out[..n as usize];
                let mut out = crate::response_header(200, array.len() + 10, false);
                out.extend_from_slice(b"{\"posts\":");
                out.extend_from_slice(array);
                out.push(b'}');
                return Ok(out);
            }
        }
        let body = self.feed_sql()?;
        Ok(crate::response_bytes(200, &body, false))
    }

    pub fn post(&mut self, id: i64) -> Result<Option<Vec<u8>>, ()> {
        if !self.reader.is_null() {
            let ids = [id];
            let mut lens = [0i32; 1];
            let mut n = 0;
            let rc = unsafe {
                if self.in_tx {
                    posts_btree_tx(
                        self.reader,
                        ids.as_ptr(),
                        1,
                        self.post_out.as_mut_ptr() as *mut c_char,
                        self.post_out.len() as c_int,
                        &mut n,
                        lens.as_mut_ptr(),
                    )
                } else {
                    posts_btree(
                        self.reader,
                        ids.as_ptr(),
                        1,
                        self.post_out.as_mut_ptr() as *mut c_char,
                        self.post_out.len() as c_int,
                        &mut n,
                        lens.as_mut_ptr(),
                    )
                }
            };
            if rc == SQLITE_OK {
                if n == 0 || lens[0] == 0 {
                    let _ = self.reopen();
                    return self.post_sql(id);
                }
                let frag = &self.post_out[..n as usize];
                let mut body = Vec::with_capacity(frag.len() + 10);
                body.extend_from_slice(b"{\"post\":");
                body.extend_from_slice(frag);
                body.push(b'}');
                return Ok(Some(body));
            }
        }
        self.post_sql(id)
    }

    pub fn post_response(&mut self, id: i64) -> Result<Option<Vec<u8>>, ()> {
        if !self.reader.is_null() {
            let ids = [id];
            let mut lens = [0i32; 1];
            let mut n = 0;
            let rc = unsafe {
                if self.in_tx {
                    posts_btree_tx(
                        self.reader,
                        ids.as_ptr(),
                        1,
                        self.post_out.as_mut_ptr() as *mut c_char,
                        self.post_out.len() as c_int,
                        &mut n,
                        lens.as_mut_ptr(),
                    )
                } else {
                    posts_btree(
                        self.reader,
                        ids.as_ptr(),
                        1,
                        self.post_out.as_mut_ptr() as *mut c_char,
                        self.post_out.len() as c_int,
                        &mut n,
                        lens.as_mut_ptr(),
                    )
                }
            };
            if rc == SQLITE_OK {
                if n == 0 || lens[0] == 0 {
                    let _ = self.reopen();
                    return Ok(self.post_sql(id)?.map(|body| crate::response_bytes(200, &body, false)));
                }
                let frag = &self.post_out[..n as usize];
                let mut out = crate::response_header(200, frag.len() + 9, false);
                out.extend_from_slice(b"{\"post\":");
                out.extend_from_slice(frag);
                out.push(b'}');
                return Ok(Some(out));
            }
        }
        Ok(self.post_sql(id)?.map(|body| crate::response_bytes(200, &body, false)))
    }

    pub(crate) fn reopen(&mut self) -> Result<(), ()> {
        let fresh = ReadDb::open(&self.path).map_err(|_| ())?;
        *self = fresh;
        Ok(())
    }


    fn feed_sql(&self) -> Result<Vec<u8>, ()> {
        const SQL: &str = r#"
SELECT json_group_array(json_object(
    'id', p.id,
    'body', p.body,
    'created_at', p.created_at,
    'author', p.author,
    'like_count', p.like_count))
FROM (
    SELECT p.id, p.body, p.created_at, u.username AS author,
           (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
      FROM posts p JOIN users u ON u.id = p.user_id
     ORDER BY p.created_at DESC, p.id DESC
     LIMIT 20
) p
"#;
        let mut st = self.db.prepare(SQL).map_err(|_| ())?;
        match st.step() {
            Ok(true) => {
                let posts = st.column_text(0);
                let mut body = Vec::with_capacity(posts.len() + 10);
                body.extend_from_slice(b"{\"posts\":");
                body.extend_from_slice(posts.as_bytes());
                body.push(b'}');
                Ok(body)
            }
            _ => Err(()),
        }
    }

    fn post_sql(&self, id: i64) -> Result<Option<Vec<u8>>, ()> {
        const SQL: &str = r#"
SELECT json_object('post', json_object(
    'id', p.id,
    'body', p.body,
    'created_at', p.created_at,
    'author', u.username,
    'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id)))
  FROM posts p JOIN users u ON u.id = p.user_id
 WHERE p.id = ?1
"#;
        let mut st = self.db.prepare(SQL).map_err(|_| ())?;
        st.bind_i64(1, id);
        match st.step() {
            Ok(true) => Ok(Some(st.column_text(0).into_bytes())),
            Ok(false) => Ok(None),
            Err(_) => Err(()),
        }
    }
}

fn load_roots(db: &Db) -> Option<(u32, u32, u32, u32)> {
    const SQL: &str =
        "SELECT rootpage FROM sqlite_master WHERE name = ?1 AND type IN ('table','index')";
    let names = [
        "posts",
        "users",
        "posts_created_at_id_idx",
        "likes_post_id_idx",
    ];
    let mut out = [0u32; 4];
    for (i, name) in names.iter().enumerate() {
        let mut st = db.prepare(SQL).ok()?;
        st.bind_text(1, name);
        if !matches!(st.step(), Ok(true)) {
            return None;
        }
        let root = st.column_i64(0);
        if root <= 0 {
            return None;
        }
        out[i] = root as u32;
    }
    Some((out[0], out[1], out[2], out[3]))
}

pub struct WriteDb {
    db: Db,
}

impl WriteDb {
    pub fn open(path: &str) -> Result<Self, String> {
        let db = Db::open(path)?;
        db.exec("PRAGMA foreign_keys=ON")?;
        Ok(Self { db })
    }

    pub fn prepare(&self, sql: &str) -> Result<Stmt<'_>, String> {
        self.db.prepare(sql)
    }

    pub(crate) fn prepare_static(&self, sql: &str) -> Result<Stmt<'static>, String> {
        let stmt = self.db.prepare(sql)?;
        Ok(unsafe { std::mem::transmute::<Stmt<'_>, Stmt<'static>>(stmt) })
    }



    pub fn changes(&self) -> i32 {
        changes(self.db.raw())
    }

    pub fn checkpoint(&self) {
        let _ = self.db.exec("PRAGMA wal_checkpoint(PASSIVE)");
    }

    pub fn health(&self) -> bool {
        let Ok(mut st) = self.db.prepare("SELECT 1") else {
            return false;
        };
        matches!(st.step(), Ok(true))
    }

    pub fn begin(&self) -> Result<(), String> {
        self.db.exec("BEGIN IMMEDIATE")
    }

    pub fn commit(&self) -> Result<(), String> {
        self.db.exec("COMMIT")
    }

    pub fn rollback(&self) -> Result<(), String> {
        self.db.exec("ROLLBACK")
    }

    pub fn create_post(&self, user_id: i64, body: &str) -> Result<(i64, String), String> {
        let mut st = self
            .db
            .prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at")?;
        st.bind_i64(1, user_id);
        st.bind_text(2, body);
        if !st.step()? {
            return Err("no post returned".to_string());
        }
        Ok((st.column_i64(0), st.column_text(1)))
    }

    /// Returns None when the post does not exist.
    pub fn like(&self, user_id: i64, post_id: i64) -> Result<Option<(bool, i64)>, String> {
        let mut exists = self.db.prepare("SELECT 1 FROM posts WHERE id = ?1")?;
        exists.bind_i64(1, post_id);
        if !exists.step()? {
            return Ok(None);
        }
        drop(exists);

        let mut insert = self.db.prepare(
            "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) \
             ON CONFLICT (user_id, post_id) DO NOTHING",
        )?;
        insert.bind_i64(1, user_id);
        insert.bind_i64(2, post_id);
        insert.step()?;
        let inserted = changes(self.db.raw()) > 0;
        drop(insert);

        let mut count = self.db.prepare("SELECT count(*) FROM likes WHERE post_id = ?1")?;
        count.bind_i64(1, post_id);
        if !count.step()? {
            return Err("like count failed".to_string());
        }
        Ok(Some((inserted, count.column_i64(0))))
    }

    pub fn like_count(&self, post_id: i64) -> Result<i64, String> {
        let mut st = self
            .db
            .prepare("SELECT count(*) FROM likes WHERE post_id = ?1")?;
        st.bind_i64(1, post_id);
        if !st.step()? {
            return Err("like count failed".to_string());
        }
        Ok(st.column_i64(0))
    }
}
