//! Copies the seed database (the SQLite file at `$SQLITE_PATH`) into a new xitdb file, once.
//! It reads SQLite's file format directly, so nothing of SQLite is linked in.
use crate::db::{self, Mix, LIKES, OPTS, POSTS, POST_HEAD, TIMELINE, TS_LEN, USERS};
use crate::store::FileCore;
use std::os::fd::IntoRawFd;
use std::os::unix::fs::MetadataExt;
use xitdb::arraylist::ArrayList;
use xitdb::counted_hashmap::CountedHashMap;
use xitdb::cursor::Writeable;
use xitdb::database::Database;
use xitdb::error::Error;

/// Identifies the source file, so a fresh copy of the seed isn't served from an older import.
pub fn stamp(source: &str) -> Result<Vec<u8>, String> {
    let m = std::fs::metadata(source).map_err(|e| format!("{source}: {e}"))?;
    let mut s = m.len().to_be_bytes().to_vec();
    s.extend_from_slice(&m.mtime().to_be_bytes());
    s.extend_from_slice(&m.mtime_nsec().to_be_bytes());
    Ok(s)
}

type R<T> = Result<T, String>;

fn bad(what: &str) -> String {
    format!("seed database: {what}")
}

struct Sqlite<'a> {
    file: &'a [u8],
    page: usize,
    usable: usize,
}

#[inline]
fn varint(p: &[u8], i: &mut usize) -> R<u64> {
    let mut v = 0u64;
    for k in 0..9 {
        let b = *p.get(*i).ok_or_else(|| bad("truncated varint"))?;
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

/// The first N columns of a record: (serial type, bytes).
#[inline]
fn columns<const N: usize>(rec: &[u8]) -> R<[(u64, &[u8]); N]> {
    let mut i = 0;
    let hsize = varint(rec, &mut i)? as usize;
    let mut body = hsize;
    let mut out = [(0u64, &rec[0..0]); N];
    for col in out.iter_mut() {
        if i >= hsize {
            return Err(bad("record has too few columns"));
        }
        let t = varint(rec, &mut i)?;
        let n = serial_len(t);
        *col = (t, rec.get(body..body + n).ok_or_else(|| bad("truncated record"))?);
        body += n;
    }
    Ok(out)
}

#[inline]
fn int(c: (u64, &[u8])) -> R<i64> {
    match c.0 {
        8 => Ok(0),
        9 => Ok(1),
        1..=6 => Ok(c.1[1..].iter().fold(c.1[0] as i8 as i64, |v, &x| v << 8 | x as i64)),
        _ => Err(bad("expected an integer column")),
    }
}

#[inline]
fn text<'a>(c: (u64, &'a [u8])) -> R<&'a [u8]> {
    if c.0 >= 13 && c.0 & 1 == 1 {
        Ok(c.1)
    } else {
        Err(bad("expected a text column"))
    }
}

/// `YYYY-MM-DDTHH:MM:SS.mmmZ`, exactly, to ms since the epoch: `format_ts` gives it back.
fn parse_ts(t: &[u8]) -> R<u64> {
    let n = |a: usize, b: usize| t[a..b].iter().try_fold(0i64, |v, &c| c.is_ascii_digit().then(|| v * 10 + (c - b'0') as i64));
    let parse = || -> Option<u64> {
        if t.len() != TS_LEN {
            return None;
        }
        let (y, m, d) = (n(0, 4)?, n(5, 7)?, n(8, 10)?);
        let (hh, mm, ss, ms) = (n(11, 13)?, n(14, 16)?, n(17, 19)?, n(20, 23)?);
        // Civil date to days (Howard Hinnant's algorithm).
        let y2 = if m <= 2 { y - 1 } else { y };
        let era = y2.div_euclid(400);
        let yoe = y2.rem_euclid(400);
        let doy = (153 * (if m > 2 { m - 3 } else { m + 9 }) + 2) / 5 + d - 1;
        let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        let days = era * 146097 + doe - 719468;
        u64::try_from((days * 86400 + hh * 3600 + mm * 60 + ss) * 1000 + ms).ok()
    };
    let v = parse().ok_or_else(|| bad("unexpected timestamp format"))?;
    if db::format_ts(v) != t[..] {
        return Err(bad("unexpected timestamp format"));
    }
    Ok(v)
}

impl<'a> Sqlite<'a> {
    fn new(file: &'a [u8]) -> R<Sqlite<'a>> {
        if file.len() < 100 || !file.starts_with(b"SQLite format 3\0") {
            return Err(bad("not a SQLite file"));
        }
        let page = match u16::from_be_bytes([file[16], file[17]]) {
            1 => 65536,
            n => n as usize,
        };
        Ok(Sqlite { file, page, usable: page - file[20] as usize })
    }

    fn page(&self, n: u32) -> R<&'a [u8]> {
        let start = (n as usize).checked_sub(1).ok_or_else(|| bad("page 0"))? * self.page;
        self.file.get(start..start + self.page).ok_or_else(|| bad("truncated file"))
    }

    /// One table's rows in rowid order: f(rowid, record).
    fn walk(&self, root: u32, f: &mut dyn FnMut(i64, &[u8]) -> R<()>) -> R<()> {
        let p = self.page(root)?;
        let hdr = if root == 1 { 100 } else { 0 };
        let u16_at = |o: usize| u16::from_be_bytes([p[o], p[o + 1]]) as usize;
        let u32_at = |o: usize| u32::from_be_bytes([p[o], p[o + 1], p[o + 2], p[o + 3]]);
        let cells = u16_at(hdr + 3);
        match p[hdr] {
            0x05 => {
                let cell = |i: usize| u16_at(hdr + 12 + 2 * i);
                for i in 0..cells {
                    self.walk(u32_at(cell(i)), f)?;
                }
                self.walk(u32_at(hdr + 8), f)
            }
            0x0d => {
                let mut spill = Vec::new();
                for k in 0..cells {
                    let mut i = u16_at(hdr + 8 + 2 * k);
                    let total = varint(p, &mut i)? as usize;
                    let rowid = varint(p, &mut i)? as i64;
                    let u = self.usable;
                    let max_local = u - 35;
                    let local = if total <= max_local {
                        total
                    } else {
                        let min = (u - 12) * 32 / 255 - 23;
                        let n = min + (total - min) % (u - 4);
                        if n <= max_local {
                            n
                        } else {
                            min
                        }
                    };
                    let here = p.get(i..i + local).ok_or_else(|| bad("truncated cell"))?;
                    if local == total {
                        f(rowid, here)?;
                        continue;
                    }
                    spill.clear();
                    spill.extend_from_slice(here);
                    let mut next = u32_at(i + local);
                    while next != 0 && spill.len() < total {
                        let o = self.page(next)?;
                        let take = (u - 4).min(total - spill.len());
                        spill.extend_from_slice(&o[4..4 + take]);
                        next = u32::from_be_bytes([o[0], o[1], o[2], o[3]]);
                    }
                    if spill.len() != total {
                        return Err(bad("truncated overflow chain"));
                    }
                    f(rowid, &spill)?;
                }
                Ok(())
            }
            _ => Err(bad("unexpected page type")),
        }
    }

    /// Root page of the table `name`.
    fn table(&self, name: &str) -> R<u32> {
        let mut root = None;
        self.walk(1, &mut |_, rec| {
            let [kind, n, _, page] = columns::<4>(rec)?;
            if text(kind)? == b"table" && text(n)? == name.as_bytes() {
                root = Some(int(page)? as u32);
            }
            Ok(())
        })?;
        root.ok_or_else(|| bad("missing table"))
    }
}

/// Maps a file for one sequential pass; the cached pages are dropped afterwards.
struct Source {
    fd: i32,
    map: *mut libc::c_void,
    len: usize,
}

impl Source {
    fn open(path: &str) -> R<Source> {
        let f = std::fs::File::open(path).map_err(|e| format!("{path}: {e}"))?;
        let len = f.metadata().map_err(|e| e.to_string())?.len() as usize;
        let fd = f.into_raw_fd();
        let map = unsafe { libc::mmap(std::ptr::null_mut(), len.max(1), libc::PROT_READ, libc::MAP_PRIVATE, fd, 0) };
        if map == libc::MAP_FAILED {
            return Err(format!("{path}: {}", std::io::Error::last_os_error()));
        }
        unsafe { libc::madvise(map, len.max(1), libc::MADV_SEQUENTIAL) };
        Ok(Source { fd, map, len })
    }
    fn bytes(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.map as *const u8, self.len) }
    }
}

impl Drop for Source {
    fn drop(&mut self) {
        unsafe {
            libc::munmap(self.map, self.len.max(1));
            libc::posix_fadvise(self.fd, 0, 0, libc::POSIX_FADV_DONTNEED);
            libc::close(self.fd);
        }
    }
}

pub fn run(source: &str, target: &str, stamp: &[u8]) -> R<()> {
    let t0 = std::time::Instant::now();
    let wal = format!("{source}-wal");
    if std::fs::metadata(&wal).is_ok_and(|m| m.len() > 0) {
        return Err(bad("its -wal file has not been checkpointed"));
    }
    let tmp = format!("{target}.tmp");
    let _ = std::fs::remove_file(&tmp);
    let file = std::fs::OpenOptions::new().read(true).write(true).create_new(true).open(&tmp).map_err(|e| format!("{tmp}: {e}"))?;
    let (core, shared) = FileCore::new(std::os::fd::AsRawFd::as_raw_fd(&file)).map_err(|e| e.to_string())?;
    let x = Database::open(core, Mix, OPTS).map_err(|e| format!("{tmp}: {e}"))?;

    let src = Source::open(source)?;
    let sq = Sqlite::new(src.bytes())?;
    let (users, posts, likes) = (sq.table("users")?, sq.table("posts")?, sq.table("likes")?);

    // A walk's errors travel through xitdb's closure as this, then back out as `failed`.
    let mut failed: Option<String> = None;
    let catch = |failed: &mut Option<String>, e: String| {
        *failed = Some(e);
        Error::Corrupted
    };
    let finish = |r: Result<(), Error>, failed: &mut Option<String>| -> R<()> {
        match (r, failed.take()) {
            (_, Some(e)) => Err(e),
            (Err(e), None) => Err(format!("{tmp}: {e}")),
            (Ok(()), None) => Ok(()),
        }
    };

    // One transaction per table: its bytes wait in memory until its commit, once.
    let r = x.run_context(|w| {
        let root = ArrayList::new_write(w)?;
        for _ in 0..4 {
            ArrayList::new_write(&root.append_cursor()?)?;
        }
        root.append(Writeable::Bytes(stamp.to_vec()))
    });
    finish(r, &mut failed)?;

    let r = x.run_context(|w| {
        let root = ArrayList::new_write(w)?;
        let list = ArrayList::new_write(&root.put_cursor(USERS)?)?;
        let mut next = 1;
        let mut rec = Vec::new();
        let mut row = |id: i64, r: &[u8]| -> R<()> {
            let [_, name, created] = columns::<3>(r)?;
            let (name, created) = (text(name)?, text(created)?);
            if created.len() != TS_LEN {
                return Err(bad("unexpected timestamp format"));
            }
            while next < id {
                list.append(Writeable::None).map_err(|e| e.to_string())?;
                next += 1;
            }
            rec.clear();
            rec.extend_from_slice(created);
            rec.extend_from_slice(name);
            list.append(Writeable::Bytes(rec.clone())).map_err(|e| e.to_string())?;
            next += 1;
            Ok(())
        };
        sq.walk(users, &mut row).map_err(|e| catch(&mut failed, e))
    });
    finish(r, &mut failed)?;

    // (created_at, id): the order of posts_created_at_id_idx, oldest first.
    let mut order: Vec<(u64, u32)> = Vec::new();
    let mut max_post = 0i64;
    let r = x.run_context(|w| {
        let root = ArrayList::new_write(w)?;
        let list = ArrayList::new_write(&root.put_cursor(POSTS)?)?;
        let mut next = 1;
        let mut rec = Vec::new();
        let mut row = |id: i64, r: &[u8]| -> R<()> {
            let [_, uid, body, created] = columns::<4>(r)?;
            let (uid, body, created) = (int(uid)?, text(body)?, text(created)?);
            order.push((parse_ts(created)?, id as u32));
            while next < id {
                list.append(Writeable::None).map_err(|e| e.to_string())?;
                next += 1;
            }
            rec.clear();
            rec.reserve(POST_HEAD + body.len());
            rec.extend_from_slice(&uid.to_be_bytes());
            rec.extend_from_slice(created);
            rec.extend_from_slice(body);
            list.append(Writeable::Bytes(rec.clone())).map_err(|e| e.to_string())?;
            next += 1;
            max_post = id;
            Ok(())
        };
        sq.walk(posts, &mut row).map_err(|e| catch(&mut failed, e))
    });
    finish(r, &mut failed)?;

    order.sort_unstable();
    let r = x.run_context(|w| {
        let root = ArrayList::new_write(w)?;
        let list = ArrayList::new_write(&root.put_cursor(TIMELINE)?)?;
        for &(_, id) in &order {
            list.append(Writeable::Uint(id as u64))?;
        }
        Ok(())
    });
    finish(r, &mut failed)?;
    drop(order);

    // Grouped by post, so each post's map is built in one go.
    let mut rows: Vec<(u32, u32, u64)> = Vec::new();
    sq.walk(likes, &mut |_, r| {
        let [uid, pid, created] = columns::<3>(r)?;
        rows.push((int(pid)? as u32, int(uid)? as u32, parse_ts(text(created)?)?));
        Ok(())
    })?;
    rows.sort_unstable();
    let r = x.run_context(|w| {
        let root = ArrayList::new_write(w)?;
        let list = ArrayList::new_write(&root.put_cursor(LIKES)?)?;
        let mut i = 0;
        for id in 1..=max_post {
            if rows.get(i).is_none_or(|r| r.0 as i64 != id) {
                list.append(Writeable::None)?;
                continue;
            }
            let map = CountedHashMap::new_write(&list.append_cursor()?)?;
            while let Some(&(pid, uid, ms)) = rows.get(i) {
                if pid as i64 != id {
                    break;
                }
                map.put_by_hash(&db::mix(uid as i64), Writeable::Uint(ms))?;
                i += 1;
            }
        }
        if i != rows.len() {
            return Err(catch(&mut failed, bad("a like of a post that doesn't exist")));
        }
        Ok(())
    });
    finish(r, &mut failed)?;
    let rows_n = rows.len();
    drop(rows);

    // Everything is written; only now does the header say so.
    shared.publish_head().map_err(|e| e.to_string())?;
    drop(x);
    file.sync_all().map_err(|e| format!("{tmp}: {e}"))?;
    drop(file);
    std::fs::rename(&tmp, target).map_err(|e| format!("{target}: {e}"))?;
    db::sync_dir(target)?;
    let size = std::fs::metadata(target).map_or(0, |m| m.len());
    eprintln!("imported {source}: {max_post} posts, {rows_n} likes, {} MB, {:.1} s", size >> 20, t0.elapsed().as_secs_f32());
    Ok(())
}
