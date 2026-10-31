//! Read paths that drive SQLite's own B-tree cursors directly.
//!
//! Compiling the shim into the same translation unit as the amalgamation lets
//! this module skip the bytecode interpreter for the two read endpoints, while
//! the pager, WAL, locking and transaction boundaries stay SQLite's.
//!
//! The feed order (`created_at DESC, id DESC`) is read from the index SQLite
//! itself maintains for that exact order, `posts_created_at_id_idx`, so no
//! assumption is made about how ids and timestamps relate: a post inserted with
//! an earlier `created_at`, a rowid gap, or a write by any other connection is
//! served correctly with no state carried between requests. Reads always start
//! from the stored index, the like counts always come from `likes_post_id_idx`,
//! and every row is read from the page in this request's transaction.
//!
//! The stored record layout is checked once at startup, and the plan's output
//! is compared against the reference SQL once at startup; any mismatch, or an
//! unreadable record, switches reads back to the ordinary SQL statements.

use std::os::raw::{c_int, c_void};

use crate::http::{put_i64, put_jstr};

#[allow(non_camel_case_types)]
type sqlite3 = c_void;

extern "C" {
    fn feed_btree(db: *mut sqlite3) -> *mut c_void;
    fn feed_cursor_size() -> c_int;
    fn feed_keyinfo(db: *mut sqlite3, nkey: c_int, nextra: c_int) -> *mut c_void;
    fn feed_keyinfo_desc(db: *mut sqlite3, nkey: c_int, nextra: c_int) -> *mut c_void;
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
    ki_newest: *mut c_void,
    scratch_posts: Box<[u8]>,
    scratch_users: Box<[u8]>,
    scratch_likes: Box<[u8]>,
    scratch_newest: Box<[u8]>,
    pub posts: Box<[u64]>,
    pub users: Box<[u64]>,
    pub likes: Box<[u64]>,
    pub newest: Box<[u64]>,
    root_posts: u32,
    root_users: u32,
    root_likes: u32,
    root_newest: u32,
    opened: bool,
}

impl Cursors {
    pub fn new(
        db: *mut sqlite3,
        roots: (u32, u32, u32, u32),
        ki_likes: *mut c_void,
        ki_newest: *mut c_void,
    ) -> Cursors {
        let sz = unsafe { feed_cursor_size() } as usize;
        let words = sz.div_ceil(8).max(1);
        Cursors {
            bt: unsafe { feed_btree(db) },
            ki_likes,
            ki_newest,
            scratch_posts: vec![0u8; SCRATCH].into_boxed_slice(),
            scratch_users: vec![0u8; SCRATCH].into_boxed_slice(),
            scratch_likes: vec![0u8; SCRATCH].into_boxed_slice(),
            scratch_newest: vec![0u8; SCRATCH].into_boxed_slice(),
            posts: vec![0u64; words].into_boxed_slice(),
            users: vec![0u64; words].into_boxed_slice(),
            likes: vec![0u64; words].into_boxed_slice(),
            newest: vec![0u64; words].into_boxed_slice(),
            root_posts: roots.0,
            root_users: roots.1,
            root_likes: roots.2,
            root_newest: roots.3,
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
            2 => self.scratch_likes.as_mut_ptr(),
            _ => self.scratch_newest.as_mut_ptr(),
        }
    }

    /// Opens the read transaction and every cursor. Idempotent.
    pub fn open(&mut self) -> Option<()> {
        if self.opened {
            return Some(());
        }
        if unsafe { feed_begin_read(self.bt, std::ptr::null_mut()) } != 0 {
            return None;
        }
        unsafe {
            let p = Self::ptr(&mut self.posts);
            let u = Self::ptr(&mut self.users);
            let l = Self::ptr(&mut self.likes);
            let n = Self::ptr(&mut self.newest);
            if feed_cursor_open(self.bt, self.root_posts, p, std::ptr::null_mut()) != 0 {
                return None;
            }
            if feed_cursor_open(self.bt, self.root_users, u, std::ptr::null_mut()) != 0 {
                feed_cursor_close(p);
                return None;
            }
            if feed_cursor_open(self.bt, self.root_likes, l, self.ki_likes) != 0 {
                feed_cursor_close(u);
                feed_cursor_close(p);
                return None;
            }
            // The index cursor needs key information: without it SQLite treats
            // the btree as a table and refuses the first step.
            if feed_cursor_open(self.bt, self.root_newest, n, self.ki_newest) != 0 {
                feed_cursor_close(l);
                feed_cursor_close(u);
                feed_cursor_close(p);
                return None;
            }
        }
        self.opened = true;
        Some(())
    }

    pub fn is_open(&self) -> bool {
        self.opened
    }

    /// Closes every cursor. SQLite's write path must not run while a read
    /// cursor of ours is positioned on a page it is about to modify.
    pub fn close(&mut self) {
        if !self.opened {
            return;
        }
        self.opened = false;
        unsafe {
            feed_cursor_close(Self::ptr(&mut self.newest));
            feed_cursor_close(Self::ptr(&mut self.likes));
            feed_cursor_close(Self::ptr(&mut self.users));
            feed_cursor_close(Self::ptr(&mut self.posts));
        }
    }

    /// The ids of the newest 20 posts, in feed order, read from
    /// `posts_created_at_id_idx`. No assumption is made about the relation
    /// between ids and timestamps: the order is the index's own key order.
    pub fn newest_ids(&mut self, ids: &mut [i64; 20]) -> Result<usize, &'static str> {
        let n = Self::ptr(&mut self.newest);
        let rc = unsafe { feed_first(n) };
        if rc < 0 {
            return Err("index first");
        }
        let mut count = 0usize;
        let mut rc = rc;
        while rc == 0 && count < ids.len() {
            let mut len = 0u32;
            let sc = self.scratch(3);
            let rec = unsafe { feed_payload(n, sc, SCRATCH as u32, &mut len) };
            if rec.is_null() {
                return Err("index record too large");
            }
            let rec = unsafe { std::slice::from_raw_parts(rec, len as usize) };
            // (created_at, id): the index's two key columns.
            let mut cols = [Col { ty: 0, ptr: std::ptr::null() }; 4];
            decode(rec, 2, &mut cols).ok_or("index decode")?;
            if !cols[1].is_int() || cols[1].int() <= 0 {
                return Err("index id type");
            }
            ids[count] = cols[1].int();
            count += 1;
            rc = unsafe { feed_next(n) };
        }
        if rc < 0 {
            return Err("index next");
        }
        for k in 1..count {
            if ids[k] >= ids[k - 1] {
                return Err("index order");
            }
        }
        Ok(count)
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

    /// Like count for one post id.
    fn like_count(&mut self, id: i64) -> Result<i64, &'static str> {
        let l = Self::ptr(&mut self.likes);
        let mut rc = unsafe { feed_index_ge(l, self.ki_likes, id) };
        if rc < 0 {
            return Err("index seek");
        }
        let mut count = 0i64;
        while rc == 0 {
            let mut len = 0u32;
            let sc = self.scratch(2);
            let rec = unsafe { feed_payload(l, sc, SCRATCH as u32, &mut len) };
            if rec.is_null() {
                return Err("index record too large");
            }
            let rec = unsafe { std::slice::from_raw_parts(rec, len as usize) };
            let mut cols = [Col { ty: 0, ptr: std::ptr::null() }; 4];
            decode(rec, 1, &mut cols).ok_or("index decode")?;
            if !cols[0].is_int() {
                return Err("index key type");
            }
            if cols[0].int() != id {
                break;
            }
            count += 1;
            rc = unsafe { feed_next(l) };
        }
        if rc < 0 {
            return Err("index next");
        }
        Ok(count)
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

    /// Appends `{"posts":[...]}` in feed order. Err(reason) = fall back to
    /// SQL, with nothing appended.
    pub fn feed_json(&mut self, out: &mut Vec<u8>) -> Result<(), &'static str> {
        let mark = out.len();
        let r = self.feed_json_into(out);
        if r.is_err() {
            out.truncate(mark);
        }
        r
    }

    fn feed_json_into(&mut self, out: &mut Vec<u8>) -> Result<(), &'static str> {
        let mut ids = [0i64; 20];
        let n = self.newest_ids(&mut ids)?;
        out.extend_from_slice(b"{\"posts\":[");
        let mut counts = [0i64; 20];
        // `range_counts` fills one slot per rowid in `lo..=hi`, so a range result
        // is indexed by (id - lo); a per-id result is indexed by feed position.
        let mut range_lo: Option<i64> = None;
        if n > 0 {
            // Recent posts are almost always consecutive rowids, and then one
            // forward range scan of likes_post_id_idx counts all of them.
            let consecutive = ids[..n].windows(2).all(|w| w[0] == w[1] + 1);
            if consecutive {
                let lo = ids[n - 1];
                let hi = ids[0];
                self.range_counts(lo, hi, &mut counts[..n])?;
                range_lo = Some(lo);
            } else {
                for k in 0..n {
                    counts[k] = self.like_count(ids[k])?;
                }
            }
        }
        let p = Self::ptr(&mut self.posts);
        let mut known: Option<i64> = None;
        for k in 0..n {
            let id = ids[k];
            // Descending consecutive ids: step back instead of a fresh descent.
            let mut positioned = false;
            if known == Some(id + 1) {
                if unsafe { feed_prev(p) } == 0 && unsafe { feed_rowid(p) } == id {
                    positioned = true;
                }
            }
            if !positioned {
                match unsafe { feed_seek(p, id) } {
                    0 => {}
                    1 => return Err("post missing"),
                    _ => return Err("post seek"),
                }
            }
            known = Some(id);
            let mut len = 0u32;
            let sc = self.scratch(0);
            let rec = unsafe { feed_payload(p, sc, SCRATCH as u32, &mut len) };
            if rec.is_null() {
                return Err("payload too large");
            }
            let rec = unsafe { std::slice::from_raw_parts(rec, len as usize) };
            let mut cols = [Col { ty: 0, ptr: std::ptr::null() }; 4];
            decode(rec, 4, &mut cols).ok_or("decode")?;
            if !cols[1].is_int() || !cols[2].is_text() || !cols[3].is_text() {
                return Err("record layout");
            }
            let author = self.user_name(cols[1].int()).ok_or("author")?;
            if k > 0 {
                out.push(b',');
            }
            let likes = match range_lo {
                Some(lo) => counts[(id - lo) as usize],
                None => counts[k],
            };
            Self::emit(out, id, cols[2].bytes(), cols[3].bytes(), author, likes);
        }
        out.extend_from_slice(b"]}");
        Ok(())
    }

    /// Appends `{"post":{...}}`. 0 ok, 1 missing, -1 fall back / error, and
    /// nothing is appended unless the result is 0.
    pub fn post_json(&mut self, id: i64, out: &mut Vec<u8>) -> i32 {
        let mark = out.len();
        let rc = self.post_json_into(id, out);
        if rc != 0 {
            out.truncate(mark);
        }
        rc
    }

    fn post_json_into(&mut self, id: i64, out: &mut Vec<u8>) -> i32 {
        let p = Self::ptr(&mut self.posts);
        match unsafe { feed_seek(p, id) } {
            0 => {}
            1 => return 1,
            _ => return -1,
        }
        let mut len = 0u32;
        let sc = self.scratch(0);
        let rec = unsafe { feed_payload(p, sc, SCRATCH as u32, &mut len) };
        if rec.is_null() {
            return -1;
        }
        let rec = unsafe { std::slice::from_raw_parts(rec, len as usize) };
        let mut cols = [Col { ty: 0, ptr: std::ptr::null() }; 4];
        if decode(rec, 4, &mut cols).is_none() {
            return -1;
        }
        if !cols[1].is_int() || !cols[2].is_text() || !cols[3].is_text() {
            return -1;
        }
        let author = match self.user_name(cols[1].int()) {
            Some(a) => a,
            None => return -1,
        };
        let likes = match self.like_count(id) {
            Ok(c) => c,
            Err(_) => return -1,
        };
        out.extend_from_slice(b"{\"post\":");
        Self::emit(out, id, cols[2].bytes(), cols[3].bytes(), author, likes);
        out.push(b'}');
        0
    }

    /// True when the stored author still exists (used to validate reads).
    pub fn author_exists(&mut self, uid: i64) -> bool {
        self.user_name(uid).is_some()
    }
}
