//! Read plans over SQLite's own pager/WAL-backed cursors. No database pages,
//! rows, counts or ordering facts survive a request in application state.
use std::{
    ffi::{c_int, c_void},
    ptr::NonNull,
    slice,
};

unsafe extern "C" {
    fn native_begin(db: *mut c_void) -> c_int;
    fn native_new(db: *mut c_void, root: u32, keys: c_int, desc: c_int) -> *mut c_void;
    fn native_open(c: *mut c_void) -> c_int;
    fn native_close(c: *mut c_void);
    fn native_free(c: *mut c_void);
    fn native_first(c: *mut c_void) -> c_int;
    fn native_next(c: *mut c_void) -> c_int;
    fn native_previous(c: *mut c_void) -> c_int;
    fn native_seek(c: *mut c_void, id: i64) -> c_int;
    fn native_rowid(c: *mut c_void) -> i64;
    fn native_ge(c: *mut c_void, id: i64) -> c_int;
    fn native_payload(c: *mut c_void, total: *mut u32, local: *mut u32) -> *const u8;
    fn native_copy(c: *mut c_void, n: u32, out: *mut c_void) -> c_int;
}
type Result<T> = std::result::Result<T, i32>;
const CORRUPT: i32 = 11;
fn row(rc: i32) -> Result<bool> {
    match rc {
        0 => Ok(true),
        101 => Ok(false),
        rc => Err(rc),
    }
}
struct Cursor {
    raw: NonNull<c_void>,
    spill: Vec<u8>,
}
impl Cursor {
    unsafe fn new(db: *mut c_void, root: u32, keys: i32, desc: bool) -> Self {
        Self {
            raw: NonNull::new(unsafe { native_new(db, root, keys, desc as i32) })
                .expect("allocate SQLite cursor"),
            spill: Vec::new(),
        }
    }
    fn open(&mut self) -> Result<()> {
        match unsafe { native_open(self.raw.as_ptr()) } {
            0 => Ok(()),
            rc => Err(rc),
        }
    }
    fn first(&mut self) -> Result<bool> {
        self.open()?;
        row(unsafe { native_first(self.raw.as_ptr()) })
    }
    fn next(&mut self) -> Result<bool> {
        row(unsafe { native_next(self.raw.as_ptr()) })
    }
    fn previous(&mut self) -> Result<bool> {
        row(unsafe { native_previous(self.raw.as_ptr()) })
    }
    fn seek(&mut self, id: i64) -> Result<bool> {
        self.open()?;
        row(unsafe { native_seek(self.raw.as_ptr(), id) })
    }
    fn ge(&mut self, id: i64) -> Result<bool> {
        self.open()?;
        row(unsafe { native_ge(self.raw.as_ptr(), id) })
    }
    fn rowid(&self) -> i64 {
        unsafe { native_rowid(self.raw.as_ptr()) }
    }
    fn record(&mut self) -> Result<&[u8]> {
        let (mut total, mut local) = (0, 0);
        // An open, positioned cursor pins its page. The slice cannot outlive a
        // mutable borrow of this cursor. Other table cursors pin separate pages.
        let p = unsafe { native_payload(self.raw.as_ptr(), &mut total, &mut local) };
        if local == total {
            if p.is_null() {
                return Err(CORRUPT);
            }
            return Ok(unsafe { slice::from_raw_parts(p, total as usize) });
        }
        self.spill.resize(total as usize, 0);
        match unsafe { native_copy(self.raw.as_ptr(), total, self.spill.as_mut_ptr().cast()) } {
            0 => Ok(&self.spill),
            rc => Err(rc),
        }
    }
    fn close(&mut self) {
        unsafe { native_close(self.raw.as_ptr()) };
        self.spill.clear();
    }
}
impl Drop for Cursor {
    fn drop(&mut self) {
        unsafe { native_free(self.raw.as_ptr()) }
    }
}

#[derive(Clone, Copy, Default)]
struct Column<'a> {
    kind: u64,
    bytes: &'a [u8],
}
impl Column<'_> {
    fn integer(self) -> Result<i64> {
        match self.kind {
            8 => Ok(0),
            9 => Ok(1),
            1..=6 => {
                let mut n = (self.bytes[0] as i8) as i64;
                for &b in &self.bytes[1..] {
                    n = (n << 8) | i64::from(b);
                }
                Ok(n)
            }
            _ => Err(CORRUPT),
        }
    }
}
fn text<'a>(c: Column<'a>) -> Result<&'a [u8]> {
    if c.kind >= 13 && c.kind & 1 != 0 {
        Ok(c.bytes)
    } else {
        Err(CORRUPT)
    }
}
fn varint(input: &[u8], at: &mut usize) -> Result<u64> {
    let mut n = 0;
    for _ in 0..8 {
        let b = *input.get(*at).ok_or(CORRUPT)?;
        *at += 1;
        n = (n << 7) | u64::from(b & 127);
        if b & 128 == 0 {
            return Ok(n);
        }
    }
    let b = *input.get(*at).ok_or(CORRUPT)?;
    *at += 1;
    Ok((n << 8) | u64::from(b))
}
fn columns<const N: usize>(input: &[u8]) -> Result<[Column<'_>; N]> {
    let mut h = 0;
    let header = usize::try_from(varint(input, &mut h)?).map_err(|_| CORRUPT)?;
    if header < h || header > input.len() {
        return Err(CORRUPT);
    }
    let mut body = header;
    let mut cols = [Column::default(); N];
    for c in &mut cols {
        let kind = varint(&input[..header], &mut h)?;
        let len = match kind {
            0 | 8 | 9 => 0,
            1..=4 => kind as usize,
            5 => 6,
            6 | 7 => 8,
            10 | 11 => return Err(CORRUPT),
            _ => usize::try_from((kind - 12) / 2).map_err(|_| CORRUPT)?,
        };
        let end = body.checked_add(len).ok_or(CORRUPT)?;
        *c = Column {
            kind,
            bytes: input.get(body..end).ok_or(CORRUPT)?,
        };
        body = end;
    }
    Ok(cols)
}

pub struct Native {
    db: *mut c_void,
    started: bool,
    posts: Cursor,
    users: Cursor,
    order: Cursor,
    likes: Cursor,
}
impl Native {
    /// Caller validates the fixed schema and owns the connection exclusively.
    pub unsafe fn new(db: *mut c_void, roots: [u32; 4]) -> Self {
        unsafe {
            Self {
                db,
                started: false,
                posts: Cursor::new(db, roots[0], 0, false),
                users: Cursor::new(db, roots[1], 0, false),
                order: Cursor::new(db, roots[2], 2, true),
                likes: Cursor::new(db, roots[3], 1, false),
            }
        }
    }
    pub fn end(&mut self) {
        self.posts.close();
        self.users.close();
        self.order.close();
        self.likes.close();
        self.started = false;
    }
    fn start(&mut self) -> Result<()> {
        if !self.started {
            let rc = unsafe { native_begin(self.db) };
            if rc != 0 {
                return Err(rc);
            }
            self.started = true;
        }
        Ok(())
    }
    fn counts(&mut self, ids: &[i64], counts: &mut [u64; 20]) -> Result<()> {
        let Some(&lo) = ids.iter().min() else {
            return Ok(());
        };
        let hi = *ids.iter().max().unwrap();
        // This is a per-request plan choice, valid for sparse IDs and arbitrary
        // timestamp order. Bound work rather than scanning a huge empty range.
        if hi.checked_sub(lo).is_some_and(|n| n < 256) {
            let mut slots = [u8::MAX; 256];
            for (i, &id) in ids.iter().enumerate() {
                slots[(id - lo) as usize] = i as u8;
            }
            let mut found = self.likes.ge(lo)?;
            while found {
                let id = columns::<1>(self.likes.record()?)?[0].integer()?;
                if id > hi {
                    break;
                }
                let offset =
                    usize::try_from(id.checked_sub(lo).ok_or(CORRUPT)?).map_err(|_| CORRUPT)?;
                let slot = *slots.get(offset).ok_or(CORRUPT)?;
                if slot != u8::MAX {
                    counts[slot as usize] += 1;
                }
                found = self.likes.next()?;
            }
        } else {
            for (i, &id) in ids.iter().enumerate() {
                let mut found = self.likes.ge(id)?;
                while found {
                    if columns::<1>(self.likes.record()?)?[0].integer()? != id {
                        break;
                    }
                    counts[i] += 1;
                    found = self.likes.next()?;
                }
            }
        }
        Ok(())
    }
    pub fn read(&mut self, id: Option<i64>, out: &mut Vec<u8>) -> Result<bool> {
        self.start()?;
        let mut ids = [0i64; 20];
        let mut n = 0;
        if let Some(id) = id {
            if !self.posts.seek(id)? {
                return Ok(false);
            }
            ids[0] = id;
            n = 1;
        } else {
            let mut found = self.order.first()?;
            while found && n < 20 {
                ids[n] = columns::<2>(self.order.record()?)?[1].integer()?;
                n += 1;
                if n < 20 {
                    found = self.order.next()?;
                }
            }
        }
        let mut likes = [0u64; 20];
        self.counts(&ids[..n], &mut likes)?;
        out.extend_from_slice(if id.is_some() {
            b"{\"post\":"
        } else {
            b"{\"posts\":["
        });
        let mut previous = None;
        for i in 0..n {
            let found = if previous.is_some() && previous == ids[i].checked_add(1) {
                self.posts.previous()?
            } else {
                self.posts.seek(ids[i])?
            };
            if !found || self.posts.rowid() != ids[i] {
                return Err(CORRUPT);
            }
            let post = columns::<4>(self.posts.record()?)?;
            let uid = post[1].integer()?;
            if !self.users.seek(uid)? {
                return Err(CORRUPT);
            }
            let author = columns::<2>(self.users.record()?)?;
            if i != 0 {
                out.push(b',');
            }
            crate::output::post(
                ids[i],
                text(post[2])?,
                text(post[3])?,
                text(author[1])?,
                likes[i],
                out,
            );
            previous = Some(ids[i]);
        }
        out.extend_from_slice(if id.is_some() { b"}" } else { b"]}" });
        Ok(true)
    }
}
