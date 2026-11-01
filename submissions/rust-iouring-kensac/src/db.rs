//! Reads through SQLite's B-tree cursors (see csrc/shim.c) and writes through SQL. Every
//! response is built from rows read while the request is served, inside the batch's
//! transaction; nothing is kept between requests except prepared statements and cursors.
use crate::json::{self, escape_into, push_int};
use std::ffi::{c_char, c_int, c_void, CString};

type Cur = *mut c_void;

unsafe extern "C" {
    fn td_open(path: *const c_char, err: *mut c_char, n: c_int) -> c_int;
    fn td_begin() -> c_int;
    fn td_commit() -> c_int;
    fn td_tick();
    fn td_writes_held() -> c_int;
    fn td_cursor(i: c_int) -> Cur;
    fn td_seek(c: Cur, id: i64) -> c_int;
    fn td_seek_ge(c: Cur, k: i64) -> c_int;
    fn td_first(c: Cur) -> c_int;
    fn td_next(c: Cur) -> c_int;
    fn td_payload(c: Cur, len: *mut u32) -> *const u8;
    fn td_count_likes(c: Cur, lo: i64, hi: i64, n: *mut u32) -> c_int;
    fn td_insert_post(uid: i64, body: *const u8, len: c_int, id: *mut i64, created: *mut u8, clen: *mut c_int) -> c_int;
    fn td_insert_like(uid: i64, pid: i64) -> c_int;
    fn td_health(err: *mut c_char, n: c_int) -> c_int;
    fn td_feed_sql_step(
        id: *mut i64,
        body: *mut *const u8,
        blen: *mut c_int,
        ca: *mut *const u8,
        clen: *mut c_int,
        author: *mut *const u8,
        alen: *mut c_int,
        likes: *mut i64,
    ) -> c_int;
}

const POSTS: c_int = 0;
const USERS: c_int = 1;
const LIKES_IDX: c_int = 3;
const FEED_LEN: usize = 20;
/// Count likes with one index range walk when the feed's ids span at most this many ids.
const RANGE_SPAN: i64 = 512;

pub struct DbError;

/// The current row of a cursor, as record bytes.
#[inline]
fn payload<'a>(c: Cur) -> Result<&'a [u8], DbError> {
    let mut n = 0u32;
    let p = unsafe { td_payload(c, &mut n) };
    if p.is_null() {
        return Err(DbError);
    }
    Ok(unsafe { std::slice::from_raw_parts(p, n as usize) })
}

#[inline(always)]
fn varint(p: &[u8], i: &mut usize) -> Result<u64, DbError> {
    // One-byte varints (every serial type below 64 bytes of text) take the short path.
    if let Some(&b) = p.get(*i) {
        if b < 0x80 {
            *i += 1;
            return Ok(b as u64);
        }
    }
    varint_long(p, i)
}

#[inline(never)]
fn varint_long(p: &[u8], i: &mut usize) -> Result<u64, DbError> {
    let mut v = 0u64;
    for k in 0..9 {
        let b = *p.get(*i).ok_or(DbError)?;
        *i += 1;
        if k == 8 {
            return Ok(v << 8 | b as u64);
        }
        v = v << 7 | (b & 0x7f) as u64;
        if b & 0x80 == 0 {
            return Ok(v);
        }
    }
    Ok(v)
}

#[inline]
fn serial_len(t: u64) -> usize {
    match t {
        0 | 8 | 9 | 10 | 11 => 0,
        1 => 1,
        2 => 2,
        3 => 3,
        4 => 4,
        5 => 6,
        6 | 7 => 8,
        t => ((t - 12) / 2) as usize,
    }
}

/// Column `t`/`bytes` as an integer (serial types 1–6, 8, 9).
#[inline]
fn int_of(t: u64, b: &[u8]) -> Result<i64, DbError> {
    Ok(match t {
        8 => 0,
        9 => 1,
        1..=6 => {
            let n = serial_len(t);
            let mut v = b[0] as i8 as i64;
            for &x in &b[1..n] {
                v = v << 8 | x as i64;
            }
            v
        }
        _ => return Err(DbError),
    })
}

/// The first N columns of a record: (serial type, bytes).
#[inline]
fn columns<const N: usize>(rec: &[u8]) -> Result<[(u64, &[u8]); N], DbError> {
    let mut i = 0;
    let hsize = varint(rec, &mut i)? as usize;
    if hsize > rec.len() {
        return Err(DbError);
    }
    let mut body = hsize;
    let mut out = [(0u64, &rec[0..0]); N];
    for col in out.iter_mut() {
        if i >= hsize {
            return Err(DbError);
        }
        let t = varint(&rec[..hsize], &mut i)?;
        let n = serial_len(t);
        let b = rec.get(body..body + n).ok_or(DbError)?;
        *col = (t, b);
        body += n;
    }
    Ok(out)
}

#[inline]
fn text(c: (u64, &[u8])) -> Result<&[u8], DbError> {
    if c.0 >= 13 && c.0 & 1 == 1 {
        Ok(c.1)
    } else {
        Err(DbError)
    }
}

/// Likes of one post.
fn count_likes(c: Cur, id: i64) -> Result<i64, DbError> {
    let mut n = 0u32;
    match unsafe { td_count_likes(c, id, id, &mut n) } {
        0 => Ok(n as i64),
        1 => count_likes_slow(c, id),
        _ => Err(DbError),
    }
}

/// Likes of one post, decoding each entry here: for cells td_count_likes doesn't expect.
fn count_likes_slow(c: Cur, id: i64) -> Result<i64, DbError> {
    let mut r = unsafe { td_seek_ge(c, id) };
    let mut n = 0;
    while r == 1 {
        let [k] = columns::<1>(payload(c)?)?;
        if int_of(k.0, k.1)? != id {
            break;
        }
        n += 1;
        r = unsafe { td_next(c) };
    }
    if r < 0 {
        return Err(DbError);
    }
    Ok(n)
}

pub enum PostLookup {
    Found,
    Missing,
}

struct Row {
    id: i64,
    user_id: i64,
    body: (u32, u32),
    created: (u32, u32),
    author: (u32, u32),
    likes: i64,
}

pub struct Db {
    in_tx: bool,
    cursors: [Cur; 4],
    rows: Vec<Row>,
    /// Copies of the feed rows' text, so each cursor can move on.
    text: Vec<u8>,
    order: Vec<usize>,
}

impl Db {
    pub fn open(path: &str) -> Result<Db, String> {
        let p = CString::new(path).map_err(|_| "bad path".to_string())?;
        let mut err = [0 as c_char; 256];
        if unsafe { td_open(p.as_ptr(), err.as_mut_ptr(), err.len() as c_int) } != 0 {
            let msg = unsafe { std::ffi::CStr::from_ptr(err.as_ptr()) };
            return Err(msg.to_string_lossy().into_owned());
        }
        let cursors = [0, 1, 2, 3].map(|i| unsafe { td_cursor(i) });
        Ok(Db { in_tx: false, cursors, rows: Vec::with_capacity(FEED_LEN), text: Vec::with_capacity(16384), order: Vec::with_capacity(FEED_LEN) })
    }

    /// Joins the batch's transaction, starting it if needed. Called by every request that
    /// touches the database, so one that SQLite rolled back mid-pass is never written to.
    #[inline]
    fn begin(&mut self) -> Result<(), DbError> {
        self.in_tx = true; // commit() must run even if this fails, to report the lost batch
        if unsafe { td_begin() } != 0 {
            return Err(DbError);
        }
        Ok(())
    }

    /// Ends the batch's transaction. false if the commit failed (and was rolled back).
    pub fn commit(&mut self) -> bool {
        if !self.in_tx {
            return true;
        }
        // td_commit also reports a transaction SQLite rolled back by itself during the pass.
        self.in_tx = false;
        unsafe { td_commit() == 0 }
    }

    /// Between passes, after the commit: drives the checkpoint thread (see csrc/shim.c).
    #[inline]
    pub fn tick(&mut self) {
        unsafe { td_tick() }
    }

    /// Writes must wait: the checkpoint thread is copying the log's last frames.
    #[inline]
    pub fn writes_held(&self) -> bool {
        unsafe { td_writes_held() != 0 }
    }

    pub fn health(&mut self) -> Result<(), String> {
        if self.begin().is_err() {
            return Err("cannot begin transaction".into());
        }
        let mut err = [0 as c_char; 256];
        if unsafe { td_health(err.as_mut_ptr(), err.len() as c_int) } != 0 {
            let msg = unsafe { std::ffi::CStr::from_ptr(err.as_ptr()) };
            return Err(msg.to_string_lossy().into_owned());
        }
        Ok(())
    }

    fn count_likes(&self, id: i64) -> Result<i64, DbError> {
        count_likes(self.cursors[LIKES_IDX as usize], id)
    }

    /// Appends the username of `user_id` to `out`, escaped. false if there is no such user.
    fn author(&self, user_id: i64, out: &mut Vec<u8>) -> Result<bool, DbError> {
        let c = self.cursors[USERS as usize];
        match unsafe { td_seek(c, user_id) } {
            1 => {}
            0 => return Ok(false),
            _ => return Err(DbError),
        }
        let [_, name] = columns::<2>(payload(c)?)?;
        escape_into(out, text(name)?);
        Ok(true)
    }

    /// `{"post":{...}}` for GET /posts/:id.
    pub fn post(&mut self, id: i64, out: &mut Vec<u8>) -> Result<PostLookup, DbError> {
        self.begin()?;
        let c = self.cursors[POSTS as usize];
        match unsafe { td_seek(c, id) } {
            1 => {}
            0 => return Ok(PostLookup::Missing),
            _ => return Err(DbError),
        }
        let [_, uid, body, created] = columns::<4>(payload(c)?)?;
        let user_id = int_of(uid.0, uid.1)?;
        let mark = out.len();
        out.extend_from_slice(b"{\"post\":{\"id\":");
        push_int(out, id);
        out.extend_from_slice(b",\"body\":\"");
        escape_into(out, text(body)?);
        out.extend_from_slice(b"\",\"created_at\":\"");
        escape_into(out, text(created)?);
        out.extend_from_slice(b"\",\"author\":\"");
        // The post's cell is no longer needed, so the users cursor may move freely now.
        if !self.author(user_id, out)? {
            out.truncate(mark); // the JOIN finds no row
            return Ok(PostLookup::Missing);
        }
        out.extend_from_slice(b"\",\"like_count\":");
        push_int(out, self.count_likes(id)?);
        out.extend_from_slice(b"}}");
        Ok(PostLookup::Found)
    }

    /// Whether post `id` exists (for likes).
    pub fn post_exists(&mut self, id: i64) -> Result<bool, DbError> {
        self.begin()?;
        match unsafe { td_seek(self.cursors[POSTS as usize], id) } {
            1 => Ok(true),
            0 => Ok(false),
            _ => Err(DbError),
        }
    }

    /// `{"posts":[...]}`: the 20 newest by (created_at DESC, id DESC), read off that index.
    pub fn feed(&mut self, out: &mut Vec<u8>) -> Result<(), DbError> {
        self.begin()?;
        if !self.feed_rows()? {
            return self.feed_sql(out); // data the B-tree plan doesn't expect: let SQL answer
        }
        self.feed_json(out);
        Ok(())
    }

    /// Fills self.rows. false if the plain query should answer instead.
    fn feed_rows(&mut self) -> Result<bool, DbError> {
        let [cp, cu, ci, cl] = self.cursors;
        self.rows.clear();
        self.text.clear();
        let t = &mut self.text;
        // 1. The index: (created_at, id) newest first.
        let mut r = unsafe { td_first(ci) };
        while r == 1 && self.rows.len() < FEED_LEN {
            let [ca, id, _] = columns::<3>(payload(ci)?)?;
            let s = t.len() as u32;
            t.extend_from_slice(text(ca)?);
            self.rows.push(Row { id: int_of(id.0, id.1)?, user_id: 0, body: (0, 0), created: (s, t.len() as u32), author: (0, 0), likes: 0 });
            r = unsafe { td_next(ci) };
        }
        if r < 0 {
            return Err(DbError);
        }
        // 2. The posts, in ascending id order: consecutive ids are one step along the leaf.
        self.order.clear();
        // Newest-first is normally descending id, so ascending is the reverse.
        self.order.extend((0..self.rows.len()).rev());
        let rows = &mut self.rows;
        if !self.order.windows(2).all(|w| rows[w[0]].id < rows[w[1]].id) {
            self.order.sort_unstable_by_key(|&i| rows[i].id);
        }
        for &i in &self.order {
            if unsafe { td_seek(cp, rows[i].id) } != 1 {
                return Ok(false); // index and table disagree
            }
            let [_, uid, body] = columns::<3>(payload(cp)?)?;
            rows[i].user_id = int_of(uid.0, uid.1)?;
            let s = t.len() as u32;
            t.extend_from_slice(text(body)?);
            rows[i].body = (s, t.len() as u32);
        }
        // 3. Authors.
        for row in rows.iter_mut() {
            match unsafe { td_seek(cu, row.user_id) } {
                1 => {}
                0 => return Ok(false), // a post without a user: the JOIN drops it
                _ => return Err(DbError),
            }
            let [_, name] = columns::<2>(payload(cu)?)?;
            let s = t.len() as u32;
            t.extend_from_slice(text(name)?);
            row.author = (s, t.len() as u32);
        }
        // 4. Like counts: one walk over likes_post_id_idx when the ids are close together.
        if let (Some(&lo), Some(&hi)) = (self.order.first(), self.order.last()) {
            let (lo, hi) = (rows[lo].id, rows[hi].id);
            if hi - lo <= RANGE_SPAN {
                let mut n = [0u32; RANGE_SPAN as usize + 1];
                match unsafe { td_count_likes(cl, lo, hi, n.as_mut_ptr()) } {
                    0 => {
                        for row in rows.iter_mut() {
                            row.likes = n[(row.id - lo) as usize] as i64;
                        }
                    }
                    1 => {
                        for row in rows.iter_mut() {
                            row.likes = count_likes_slow(cl, row.id)?;
                        }
                    }
                    _ => return Err(DbError),
                }
            } else {
                for row in rows.iter_mut() {
                    row.likes = count_likes(cl, row.id)?;
                }
            }
        }
        Ok(true)
    }

    #[cfg(feature = "bench")]
    pub fn bench_feed_json(&self, out: &mut Vec<u8>) {
        self.feed_json(out)
    }

    /// Written in one reservation: every row's strings live in self.text, so 6x its length
    /// bounds their escaped form.
    fn feed_json(&self, out: &mut Vec<u8>) {
        const ROW: usize = b",{\"id\":,\"body\":\"\",\"created_at\":\"\",\"author\":\"\",\"like_count\":}".len() + 2 * json::INT_BOUND;
        let t = &self.text;
        let bound = b"{\"posts\":[]}".len() + self.rows.len() * ROW + json::esc_bound(t.len());
        json::fill(out, bound, |c| {
            c.lit(b"{\"posts\":[");
            for (k, row) in self.rows.iter().enumerate() {
                if k > 0 {
                    c.byte(b',');
                }
                c.lit(b"{\"id\":");
                c.int(row.id);
                c.lit(b",\"body\":\"");
                c.esc(&t[row.body.0 as usize..row.body.1 as usize]);
                c.lit(b"\",\"created_at\":\"");
                c.esc(&t[row.created.0 as usize..row.created.1 as usize]);
                c.lit(b"\",\"author\":\"");
                c.esc(&t[row.author.0 as usize..row.author.1 as usize]);
                c.lit(b"\",\"like_count\":");
                c.int(row.likes);
                c.byte(b'}');
            }
            c.lit(b"]}");
        });
    }

    /// The reference query, for data the B-tree plan doesn't expect.
    fn feed_sql(&mut self, out: &mut Vec<u8>) -> Result<(), DbError> {
        out.extend_from_slice(b"{\"posts\":[");
        let mut first = true;
        loop {
            let (mut id, mut likes) = (0i64, 0i64);
            let (mut b, mut c, mut a) = (std::ptr::null(), std::ptr::null(), std::ptr::null());
            let (mut bl, mut cl, mut al) = (0, 0, 0);
            let r = unsafe { td_feed_sql_step(&mut id, &mut b, &mut bl, &mut c, &mut cl, &mut a, &mut al, &mut likes) };
            if r == 0 {
                break;
            }
            if r < 0 {
                return Err(DbError);
            }
            let s = |p: *const u8, n: c_int| if p.is_null() { &[][..] } else { unsafe { std::slice::from_raw_parts(p, n as usize) } };
            if !first {
                out.push(b',');
            }
            first = false;
            out.extend_from_slice(b"{\"id\":");
            push_int(out, id);
            out.extend_from_slice(b",\"body\":\"");
            escape_into(out, s(b, bl));
            out.extend_from_slice(b"\",\"created_at\":\"");
            escape_into(out, s(c, cl));
            out.extend_from_slice(b"\",\"author\":\"");
            escape_into(out, s(a, al));
            out.extend_from_slice(b"\",\"like_count\":");
            push_int(out, likes);
            out.push(b'}');
        }
        out.extend_from_slice(b"]}");
        Ok(())
    }

    /// INSERT a post. Returns (id, created_at).
    pub fn insert_post(&mut self, user_id: i64, body: &[u8]) -> Result<(i64, [u8; 64], usize), DbError> {
        self.begin()?;
        let mut id = 0i64;
        let mut created = [0u8; 64];
        let mut n: c_int = 0;
        if unsafe { td_insert_post(user_id, body.as_ptr(), body.len() as c_int, &mut id, created.as_mut_ptr(), &mut n) } != 0 {
            return Err(DbError);
        }
        Ok((id, created, n as usize))
    }

    /// true if the like is new.
    pub fn insert_like(&mut self, user_id: i64, post_id: i64) -> Result<bool, DbError> {
        self.begin()?;
        match unsafe { td_insert_like(user_id, post_id) } {
            1 => Ok(true),
            0 => Ok(false),
            _ => Err(DbError),
        }
    }
}
