//! Read paths that drive SQLite's own B-tree cursors directly.
//!
//! Compiling the shim into the same translation unit as the amalgamation lets
//! this module skip the bytecode interpreter for the two read endpoints, while
//! the pager, WAL, locking and transaction boundaries stay SQLite's.
//!
//! Correctness rests on two facts that are checked, not assumed:
//!   * the stored schema is the one these record layouts were written for, and
//!   * post rowids are dense and `created_at` never decreases in rowid order,
//!     so the feed order (`created_at DESC, id DESC`) is reverse rowid order.
//! Both are verified at startup and after every insert; any deviation, any
//! foreign write, or any overflow-payload row permanently switches reads back
//! to the ordinary SQL statements.

use std::os::raw::{c_int, c_void};

use crate::http::{put_i64, put_jstr};

#[allow(non_camel_case_types)]
type sqlite3 = c_void;

extern "C" {
    fn feed_btree(db: *mut sqlite3) -> *mut c_void;
    fn feed_cursor_size() -> c_int;
    fn feed_keyinfo(db: *mut sqlite3, nkey: c_int, nextra: c_int) -> *mut c_void;
    fn feed_begin_read(bt: *mut c_void, version: *mut u32) -> c_int;
    fn feed_cursor_open(bt: *mut c_void, root: u32, mem: *mut c_void, ki: *mut c_void) -> c_int;
    fn feed_cursor_close(mem: *mut c_void) -> c_int;
    fn feed_first(mem: *mut c_void) -> c_int;
    fn feed_last(mem: *mut c_void) -> c_int;
    fn feed_next(mem: *mut c_void) -> c_int;
    fn feed_prev(mem: *mut c_void) -> c_int;
    fn feed_rowid(mem: *mut c_void) -> i64;
    fn feed_seek(mem: *mut c_void, rowid: i64) -> c_int;
    fn feed_index_ge(mem: *mut c_void, ki: *mut c_void, k: i64) -> c_int;
    fn feed_payload(mem: *mut c_void, scratch: *mut u8, cap: u32, len: *mut u32)
        -> *const u8;
}

const SCRATCH: usize = 16 * 1024;
const ROW_IS_INT: u8 = 1;
const ROW_IS_TEXT: u8 = 2;

#[derive(Clone, Copy)]
struct Col {
    ty: u64,
    ptr: *const u8,
}

impl Col {
    fn is_int(&self) -> bool {
        matches!(self.ty, 1..=6 | 8 | 9)
    }
    fn is_text(&self) -> bool {
        self.ty >= 13 && self.ty & 1 == 1
    }
    fn len(&self) -> usize {
        serial_len(self.ty)
    }
    fn int(&self) -> i64 {
        let p = self.ptr;
        unsafe {
            match self.ty {
                1 => *p as i8 as i64,
                2 => i16::from_be_bytes([*p, *p.add(1)]) as i64,
                3 => {
                    let v = u32::from_be_bytes([*p, *p.add(1), *p.add(2), 0]) >> 8;
                    (v as i32) as i64
                }
                4 => i32::from_be_bytes([*p, *p.add(1), *p.add(2), *p.add(3)]) as i64,
                5 => {
                    let hi = i16::from_be_bytes([*p, *p.add(1)]) as i64;
                    let lo = u32::from_be_bytes([*p.add(2), *p.add(3), *p.add(4), *p.add(5)]);
                    (hi << 32) | lo as i64
                }
                6 => i64::from_be_bytes([
                    *p,
                    *p.add(1),
                    *p.add(2),
                    *p.add(3),
                    *p.add(4),
                    *p.add(5),
                    *p.add(6),
                    *p.add(7),
                ]),
                8 => 0,
                9 => 1,
                _ => 0,
            }
        }
    }
    fn bytes(&self) -> &'static [u8] {
        let n = self.len();
        if n == 0 || self.ptr.is_null() {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(self.ptr, n) }
        }
    }
}

fn serial_len(t: u64) -> usize {
    match t {
        0 => 0,
        1 => 1,
        2 => 2,
        3 => 3,
        4 => 4,
        5 => 6,
        6 | 7 => 8,
        8 | 9 => 0,
        10 | 11 => 0,
        _ => ((t - 12) / 2) as usize,
    }
}

fn read_varint(p: &[u8], at: &mut usize) -> Option<u64> {
    let mut v: u64 = 0;
    for i in 0..8 {
        let b = *p.get(*at + i)?;
        v = (v << 7) | (b & 0x7f) as u64;
        if b & 0x80 == 0 {
            *at += i + 1;
            return Some(v);
        }
    }
    let b = *p.get(*at + 8)?;
    *at += 9;
    Some((v << 8) | b as u64)
}

/// Decode up to `ncol` columns of a record image.
fn decode(rec: &[u8], ncol: usize, cols: &mut [Col; 4]) -> Option<()> {
    let mut at = 0usize;
    let hsz = read_varint(rec, &mut at)? as usize;
    if hsz > rec.len() || at > hsz {
        return None;
    }
    let mut h = at;
    let mut body = hsz;
    for slot in cols.iter_mut().take(ncol) {
        let t = read_varint(rec, &mut h)?;
        if h > hsz {
            return None;
        }
        let len = serial_len(t);
        if body + len > rec.len() {
            return None;
        }
        *slot = Col { ty: t, ptr: unsafe { rec.as_ptr().add(body) } };
        body += len;
    }
    Some(())
}

pub struct Cursors {
    bt: *mut c_void,
    ki_likes: *mut c_void,
    scratch_posts: Box<[u8]>,
    scratch_users: Box<[u8]>,
    scratch_likes: Box<[u8]>,
    pub posts: Box<[u64]>,
    pub users: Box<[u64]>,
    pub likes: Box<[u64]>,
    root_posts: u32,
    root_users: u32,
    root_likes: u32,
    opened: bool,
}

/// The two invariants the feed plan needs, plus the schema check.
pub struct Plan {
    pub schema_ok: bool,
    pub invariant_ok: bool,
    pub last_created: Vec<u8>,
    pub have_last: bool,
}

impl Cursors {
    pub fn new(db: *mut sqlite3, roots: (u32, u32, u32), ki_likes: *mut c_void) -> Cursors {
        let sz = unsafe { feed_cursor_size() } as usize;
        let words = sz.div_ceil(8).max(1);
        Cursors {
            bt: unsafe { feed_btree(db) },
            ki_likes,
            scratch_posts: vec![0u8; SCRATCH].into_boxed_slice(),
            scratch_users: vec![0u8; SCRATCH].into_boxed_slice(),
            scratch_likes: vec![0u8; SCRATCH].into_boxed_slice(),
            posts: vec![0u64; words].into_boxed_slice(),
            users: vec![0u64; words].into_boxed_slice(),
            likes: vec![0u64; words].into_boxed_slice(),
            root_posts: roots.0,
            root_users: roots.1,
            root_likes: roots.2,
            opened: false,
        }
    }

    pub fn bt(&self) -> *mut c_void {
        self.bt
    }

    fn ptr(buf: &mut [u64]) -> *mut c_void {
        buf.as_mut_ptr() as *mut c_void
    }

    /// Scratch buffer for records that are not entirely on a page: 0 posts,
    /// 1 users, 2 likes.
    fn scratch(&mut self, which: u8) -> *mut u8 {
        match which {
            0 => self.scratch_posts.as_mut_ptr(),
            1 => self.scratch_users.as_mut_ptr(),
            _ => self.scratch_likes.as_mut_ptr(),
        }
    }

    /// Opens the read transaction and every cursor. Returns the data version.
    pub fn open(&mut self) -> Option<u32> {
        let mut v: u32 = 0;
        if unsafe { feed_begin_read(self.bt, &mut v) } != 0 {
            return None;
        }
        unsafe {
            let p = Self::ptr(&mut self.posts);
            if feed_cursor_open(self.bt, self.root_posts, p, std::ptr::null_mut()) != 0 {
                return None;
            }
            let u = Self::ptr(&mut self.users);
            if feed_cursor_open(self.bt, self.root_users, u, std::ptr::null_mut()) != 0 {
                feed_cursor_close(p);
                return None;
            }
            let l = Self::ptr(&mut self.likes);
            if feed_cursor_open(self.bt, self.root_likes, l, self.ki_likes) != 0 {
                feed_cursor_close(p);
                feed_cursor_close(u);
                return None;
            }
        }
        self.opened = true;
        Some(v)
    }

    pub fn close(&mut self) {
        if !self.opened {
            return;
        }
        self.opened = false;
        unsafe {
            feed_cursor_close(Self::ptr(&mut self.likes));
            feed_cursor_close(Self::ptr(&mut self.users));
            feed_cursor_close(Self::ptr(&mut self.posts));
        }
    }

    /// Sequential crawl used once at startup: dense rowids and non-decreasing
    /// `created_at` in rowid order. Returns (holds, last created, any rows, next id).
    pub fn check_invariant(&mut self) -> Option<(bool, Vec<u8>, bool, i64)> {
        let p = Self::ptr(&mut self.posts);
        let mut expect: i64 = 1;
        let mut prev: Vec<u8> = Vec::new();
        let mut have_prev = false;
        let mut rc = unsafe { feed_first(p) };
        while rc == 0 {
            if unsafe { feed_rowid(p) } != expect {
                return Some((false, Vec::new(), false, 1));
            }
            let mut len = 0u32;
            let sc = self.scratch(0);
            let rec = unsafe { feed_payload(p, sc, SCRATCH as u32, &mut len) };
            if rec.is_null() {
                return None;
            }
            let rec = unsafe { std::slice::from_raw_parts(rec, len as usize) };
            let mut cols = [Col { ty: 0, ptr: std::ptr::null() }; 4];
            decode(rec, 4, &mut cols)?;
            if !cols[1].is_int() || !cols[3].is_text() {
                return Some((false, Vec::new(), false, 1));
            }
            let created = cols[3].bytes().to_vec();
            if have_prev && prev.as_slice() > created.as_slice() {
                return Some((false, Vec::new(), false, 1));
            }
            prev = created;
            have_prev = true;
            expect += 1;
            rc = unsafe { feed_next(p) };
        }
        if rc < 0 {
            return None;
        }
        Some((true, prev, have_prev, expect))
    }

    fn user_name(&mut self, uid: i64) -> Option<&'static [u8]> {
        let u = Self::ptr(&mut self.users);
        match unsafe { feed_seek(u, uid) } {
            0 => {}
            1 => return None,
            _ => return None,
        }
        let mut len = 0u32;
        let sc = self.scratch(1);
        let rec = unsafe { feed_payload(u, sc, SCRATCH as u32, &mut len) };
        if rec.is_null() {
            return None;
        }
        let rec = unsafe { std::slice::from_raw_parts(rec, len as usize) };
        let mut cols = [Col { ty: 0, ptr: std::ptr::null() }; 4];
        decode(rec, 3, &mut cols)?;
        if !cols[1].is_text() {
            return None;
        }
        Some(cols[1].bytes())
    }

    /// Like counts for post ids in `lo..=hi`, from one scan of the index.
    fn range_counts(&mut self, lo: i64, hi: i64, out: &mut [i64]) -> Result<(), &'static str> {
        for c in out.iter_mut() {
            *c = 0;
        }
        let l = Self::ptr(&mut self.likes);
        let mut rc = unsafe { feed_index_ge(l, self.ki_likes, lo) };
        if rc < 0 {
            return Err("index seek");
        }
        while rc == 0 {
            let mut len = 0u32;
            let sc = self.scratch(2);
            let rec = unsafe { feed_payload(l, sc, SCRATCH as u32, &mut len) };
            if rec.is_null() {
                return Err("index payload too large");
            }
            let rec = unsafe { std::slice::from_raw_parts(rec, len as usize) };
            let mut cols = [Col { ty: 0, ptr: std::ptr::null() }; 4];
            decode(rec, 1, &mut cols).ok_or("index decode")?;
            if !cols[0].is_int() {
                return Err("index key type");
            }
            let pid = cols[0].int();
            if pid > hi {
                return Ok(());
            }
            if pid >= lo {
                let slot = (pid - lo) as usize;
                if slot < out.len() {
                    out[slot] += 1;
                }
            }
            rc = unsafe { feed_next(l) };
        }
        if rc < 0 {
            Err("index next")
        } else {
            Ok(())
        }
    }

    fn emit(out: &mut Vec<u8>, id: i64, body: &[u8], created: &[u8], author: &[u8], likes: i64) {
        out.extend_from_slice(b"{\"id\":");
        put_i64(out, id);
        out.extend_from_slice(b",\"body\":");
        put_jstr(out, body);
        out.extend_from_slice(b",\"created_at\":");
        put_jstr(out, created);
        out.extend_from_slice(b",\"author\":");
        put_jstr(out, author);
        out.extend_from_slice(b",\"like_count\":");
        put_i64(out, likes);
        out.push(b'}');
    }

    /// `{"posts":[...]}` in feed order. Err(reason) = fall back to SQL.
    pub fn feed_json(&mut self) -> Result<Vec<u8>, &'static str> {
        let p = Self::ptr(&mut self.posts);
        let mut out = Vec::with_capacity(16 * 1024);
        out.extend_from_slice(b"{\"posts\":[");
        let rc = unsafe { feed_last(p) };
        if rc < 0 {
            return Err("last");
        }
        if rc == 0 {
            let hi = unsafe { feed_rowid(p) };
            let lo = if hi > 20 { hi - 19 } else { 1 };
            let span = (hi - lo + 1) as usize;
            let mut counts = [0i64; 20];
            self.range_counts(lo, hi, &mut counts[..span])?;
            let mut first = true;
            let mut k = 0i64;
            loop {
                let mut len = 0u32;
                let sc = self.scratch(0);
                let rec = unsafe { feed_payload(p, sc, SCRATCH as u32, &mut len) };
                if rec.is_null() {
                    return Err("payload too large");
                }
                let rec = unsafe { std::slice::from_raw_parts(rec, len as usize) };
                let mut cols = [Col { ty: 0, ptr: std::ptr::null() }; 4];
                decode(rec, 4, &mut cols).ok_or("decode")?;
                if !cols[1].is_int() {
                    return Err("user_id type");
                }
                if !cols[2].is_text() {
                    return Err("body type");
                }
                if !cols[3].is_text() {
                    return Err("created type");
                }
                let id = unsafe { feed_rowid(p) };
                if id != hi - k {
                    return Err("rowid gap");
                }
                let author = self.user_name(cols[1].int()).ok_or("author")?;
                if !first {
                    out.push(b',');
                }
                first = false;
                Self::emit(
                    &mut out,
                    id,
                    cols[2].bytes(),
                    cols[3].bytes(),
                    author,
                    counts[(id - lo) as usize],
                );
                if k == hi - lo {
                    break;
                }
                if unsafe { feed_prev(p) } != 0 {
                    return Err("prev");
                }
                k += 1;
            }
        }
        out.extend_from_slice(b"]}");
        Ok(out)
    }

    /// `{"post":{...}}`. 0 ok, 1 missing, -1 fall back / error.
    pub fn post_json(&mut self, id: i64) -> (i32, Vec<u8>) {
        let p = Self::ptr(&mut self.posts);
        let mut out = Vec::with_capacity(1024);
        match unsafe { feed_seek(p, id) } {
            0 => {}
            1 => return (1, out),
            _ => return (-1, out),
        }
        let mut len = 0u32;
        let sc = self.scratch(0);
        let rec = unsafe { feed_payload(p, sc, SCRATCH as u32, &mut len) };
        if rec.is_null() {
            return (-1, out);
        }
        let rec = unsafe { std::slice::from_raw_parts(rec, len as usize) };
        let mut cols = [Col { ty: 0, ptr: std::ptr::null() }; 4];
        if decode(rec, 4, &mut cols).is_none() {
            return (-1, out);
        }
        if !cols[1].is_int() || !cols[2].is_text() || !cols[3].is_text() {
            return (-1, out);
        }
        let author = match self.user_name(cols[1].int()) {
            Some(a) => a,
            None => return (-1, out),
        };
        let mut counts = [0i64; 1];
        if self.range_counts(id, id, &mut counts).is_err() {
            return (-1, out);
        }
        out.extend_from_slice(b"{\"post\":");
        Self::emit(&mut out, id, cols[2].bytes(), cols[3].bytes(), author, counts[0]);
        out.push(b'}');
        (0, out)
    }

    /// True when the stored author still exists (used to validate reads).
    pub fn author_exists(&mut self, uid: i64) -> bool {
        self.user_name(uid).is_some()
    }
}
