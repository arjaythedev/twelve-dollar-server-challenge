//! The database file, as xitdb's storage backend, and the thread that makes commits durable.
//!
//! Reads are `memcpy`s from one shared read-only mapping of the file. A transaction's writes
//! collect in a heap buffer (they are appends, plus overwrites of what the transaction itself
//! just appended) and reach the file in a single `pwrite` at commit.
//!
//! xitdb commits by appending a trailer and then rewriting the root header at the start of the
//! file, and it doesn't sync after that rewrite, so a commit is not durable even with its
//! `fsync` option on. Here the header lives in memory (`Shared::head`), and only the sync thread
//! writes it to the file, in order: fdatasync the data, write the header, fdatasync again. The
//! header on disk therefore never points at anything that isn't on disk. The committed size is
//! in that header, and is all that counts: whatever lies past it in the file gets written over.
use std::cell::Cell;
use std::io;
use std::os::fd::RawFd;
use std::rc::Rc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use xitdb::core::Core;
use xitdb::error::{Error, Result};
use xitdb::slot::{Slot, SLOT_LEN};
use xitdb::tag::Tag;

/// xitdb's 20-byte file header plus its 17-byte root header (root slot and committed size).
pub const HEAD: usize = 37;
const ROOT_START: usize = 20;
/// Address space for the file: mapping past the end costs nothing, and pages become readable as
/// the file grows, without remapping.
const MAP_LEN: usize = 1 << 40;
/// The file is kept this far ahead of the commits, in real zeros. A commit that grows a file
/// makes `fdatasync` commit the filesystem's journal, as does one into `fallocate`'s unwritten
/// extents. Overwriting written blocks needs neither: only the data and a cache flush. Larger
/// steps cost a slower round each (32 MB: 60 ms).
const AHEAD: u64 = 4 << 20;

/// What the backend shares with the server: `Database` keeps its backend to itself.
pub struct Shared {
    fd: RawFd,
    map: *const u8,
    head: Cell<[u8; HEAD]>,
    /// Bytes in the file (the mapping is valid below this).
    flushed: Cell<u64>,
}

/// Reads walk xitdb's file format directly in the mapping, the way a SQLite server might walk its
/// B-trees without the SQL engine: a list lookup through `ArrayList` is about 130 ns, mostly the
/// checks and copies around each of its 5 or 6 tiers, and a feed takes 80 of them. The format is
/// stable and shared by every xitdb implementation. Writes and commits all go through the crate.
/// None means the file doesn't look like it should.
impl Shared {
    /// `N` bytes at `pos`, all below the end of the file.
    #[inline]
    fn at<const N: usize>(&self, pos: u64) -> Option<&'static [u8; N]> {
        if pos.checked_add(N as u64)? > self.flushed.get() {
            return None;
        }
        Some(unsafe { &*self.map.add(pos as usize).cast() })
    }

    #[inline]
    fn u64_at(&self, pos: u64) -> Option<u64> {
        self.at::<8>(pos).map(|b| u64::from_be_bytes(*b))
    }

    #[inline]
    fn slot_at(&self, pos: u64) -> Option<Slot> {
        Some(Slot::from_bytes(*self.at::<SLOT_LEN>(pos)?))
    }

    /// The bytes of a `Bytes` slot. The file only ever grows, and the mapping lives as long as
    /// the process.
    #[inline]
    pub fn bytes(&self, s: Slot) -> Option<&'static [u8]> {
        if s.tag != Tag::Bytes {
            return None;
        }
        let pos = u64::try_from(s.value).ok()?;
        let size = self.u64_at(pos)?;
        let body = pos + 8;
        if size > self.flushed.get() - body {
            return None;
        }
        Some(unsafe { std::slice::from_raw_parts(self.map.add(body as usize), size as usize) })
    }

    /// Length of an `ArrayList`: its header is the size, then the position of the top block.
    #[inline]
    pub fn list_len(&self, list: Slot) -> Option<u64> {
        if list.tag != Tag::ArrayList {
            return None;
        }
        self.u64_at(u64::try_from(list.value).ok()?)
    }

    /// Element `index` of an `ArrayList`, an empty slot past its end. The list is a tree of
    /// blocks of 16 slots, floor(log16(size - 1)) tiers above the leaves, indexed by the digits
    /// of `index` in base 16.
    #[inline]
    pub fn list_get(&self, list: Slot, index: u64) -> Option<Slot> {
        const EMPTY: Slot = Slot { value: 0, tag: Tag::None, full: false };
        if list.tag != Tag::ArrayList {
            return None;
        }
        let header = u64::try_from(list.value).ok()?;
        let size = self.u64_at(header)?;
        if index >= size {
            return Some(EMPTY);
        }
        let mut block = self.u64_at(header + 8)?;
        let mut shift = if size <= 16 { 0 } else { (63 - (size - 1).leading_zeros()) / 4 * 4 };
        loop {
            let s = self.slot_at(block + (index >> shift & 15) * SLOT_LEN as u64)?;
            if shift == 0 {
                return Some(s);
            }
            if s.tag != Tag::Index {
                return None;
            }
            block = u64::try_from(s.value).ok()?;
            shift -= 4;
        }
    }

    /// Entries of a `CountedHashMap`, which keeps its size in front of its top block. An empty
    /// slot is a map that was never written.
    #[inline]
    pub fn map_len(&self, map: Slot) -> Option<u64> {
        match map.tag {
            Tag::None => Some(0),
            Tag::CountedHashMap => self.u64_at(u64::try_from(map.value).ok()?),
            _ => None,
        }
    }

    pub fn head(&self) -> [u8; HEAD] {
        self.head.get()
    }

    /// Writes the whole header to the file. Only for a new database: nothing is serving it yet.
    pub fn publish_head(&self) -> io::Result<()> {
        pwrite_all(self.fd, &self.head.get(), 0)
    }
}

impl Drop for Shared {
    fn drop(&mut self) {
        unsafe { libc::munmap(self.map as *mut libc::c_void, MAP_LEN) };
    }
}

pub struct FileCore {
    fd: RawFd,
    map: *const u8,
    pos: u64,
    len: u64,
    flushed: u64,
    /// The bytes after `flushed`.
    tail: Vec<u8>,
    shared: Rc<Shared>,
}

fn pwrite_all(fd: RawFd, mut b: &[u8], mut off: u64) -> io::Result<()> {
    while !b.is_empty() {
        let n = unsafe { libc::pwrite(fd, b.as_ptr().cast(), b.len(), off as libc::off_t) };
        if n < 0 {
            let e = io::Error::last_os_error();
            if e.kind() == io::ErrorKind::Interrupted {
                continue;
            }
            return Err(e);
        }
        b = &b[n as usize..];
        off += n as u64;
    }
    Ok(())
}

impl FileCore {
    /// `fd` stays open for the life of the process.
    pub fn new(fd: RawFd) -> io::Result<(FileCore, Rc<Shared>)> {
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::fstat(fd, &mut st) } != 0 {
            return Err(io::Error::last_os_error());
        }
        let st_len = st.st_size as u64;
        let map = unsafe { libc::mmap(std::ptr::null_mut(), MAP_LEN, libc::PROT_READ, libc::MAP_SHARED | libc::MAP_NORESERVE, fd, 0) };
        if map == libc::MAP_FAILED {
            return Err(io::Error::last_os_error());
        }
        let map = map as *const u8;
        let mut head = [0u8; HEAD];
        let mut len = st_len;
        if len >= HEAD as u64 {
            unsafe { std::ptr::copy_nonoverlapping(map, head.as_mut_ptr(), HEAD) };
            // Past the committed size is space allocated ahead, or what a crash left behind.
            len = committed_size(&head).min(len);
        }
        // A new file's first bytes are the header, so its tail starts right behind that.
        let flushed = len.max(HEAD as u64);
        let shared = Rc::new(Shared { fd, map, head: Cell::new(head), flushed: Cell::new(flushed) });
        Ok((FileCore { fd, map, pos: 0, len, flushed, tail: Vec::new(), shared: shared.clone() }, shared))
    }
}

impl Core for FileCore {
    #[inline]
    fn read(&mut self, buf: &mut [u8]) -> Result<()> {
        let n = buf.len();
        let end = self.pos + n as u64;
        if end > self.len {
            return Err(Error::Truncated);
        }
        if self.pos >= HEAD as u64 && end <= self.flushed {
            // Every read outside a transaction, and nearly all inside one.
            unsafe { std::ptr::copy_nonoverlapping(self.map.add(self.pos as usize), buf.as_mut_ptr(), n) };
            self.pos = end;
            return Ok(());
        }
        let mut done = 0;
        while done < n {
            let at = self.pos as usize;
            let left = n - done;
            let k = if at < HEAD {
                let k = left.min(HEAD - at);
                buf[done..done + k].copy_from_slice(&self.shared.head.get()[at..at + k]);
                k
            } else if self.pos < self.flushed {
                let k = left.min((self.flushed - self.pos) as usize);
                unsafe { std::ptr::copy_nonoverlapping(self.map.add(at), buf[done..].as_mut_ptr(), k) };
                k
            } else {
                let off = (self.pos - self.flushed) as usize;
                buf[done..].copy_from_slice(self.tail.get(off..off + left).ok_or(Error::Truncated)?);
                left
            };
            done += k;
            self.pos += k as u64;
        }
        Ok(())
    }

    #[inline]
    fn write(&mut self, buf: &[u8]) -> Result<()> {
        let n = buf.len();
        let end = self.pos + n as u64;
        if self.pos >= self.flushed {
            let off = (self.pos - self.flushed) as usize;
            if off + n > self.tail.len() {
                self.tail.resize(off + n, 0);
            }
            self.tail[off..off + n].copy_from_slice(buf);
        } else if end <= HEAD as u64 {
            let mut head = self.shared.head.get();
            head[self.pos as usize..end as usize].copy_from_slice(buf);
            self.shared.head.set(head);
        } else {
            // Committed bytes are immutable, so xitdb has no reason to come here.
            return Err(Error::Corrupted);
        }
        self.pos = end;
        self.len = self.len.max(end);
        Ok(())
    }

    #[inline]
    fn committed_len(&mut self) -> Result<u64> {
        Ok(self.len)
    }

    #[inline]
    fn seek_to(&mut self, pos: u64) -> Result<()> {
        self.pos = pos;
        Ok(())
    }

    #[inline]
    fn position(&mut self) -> Result<u64> {
        Ok(self.pos)
    }

    fn truncate(&mut self, len: u64) -> Result<()> {
        if len >= self.flushed {
            self.tail.truncate((len - self.flushed) as usize);
        } else {
            if unsafe { libc::ftruncate(self.fd, len as libc::off_t) } != 0 {
                return Err(Error::Io(io::Error::last_os_error()));
            }
            self.tail.clear();
            self.flushed = len.max(HEAD as u64);
            self.shared.flushed.set(self.flushed);
        }
        self.len = len;
        self.pos = self.pos.min(len);
        Ok(())
    }

    fn flush(&mut self) -> Result<()> {
        Ok(())
    }

    /// xitdb calls this just before it rewrites the root header: the transaction's bytes go to
    /// the file (page cache). The sync thread makes them durable.
    fn sync(&mut self) -> Result<()> {
        if !self.tail.is_empty() {
            pwrite_all(self.fd, &self.tail, self.flushed).map_err(Error::Io)?;
            self.flushed += self.tail.len() as u64;
            self.shared.flushed.set(self.flushed);
            self.tail.clear();
        }
        Ok(())
    }
}

/// Writes AHEAD bytes of zeros at `off`. Nothing depends on them: if this fails, for instance on
/// a full disk, the commits go on extending the file.
fn write_zeros(fd: RawFd, off: u64) -> io::Result<()> {
    static ZEROS: [u8; 1 << 16] = [0; 1 << 16];
    let mut done = 0;
    while done < AHEAD {
        pwrite_all(fd, &ZEROS, off + done)?;
        done += ZEROS.len() as u64;
    }
    Ok(())
}

fn committed_size(head: &[u8; HEAD]) -> u64 {
    let mut b = [0u8; 8];
    b.copy_from_slice(&head[HEAD - 8..]);
    u64::from_be_bytes(b)
}

struct Round {
    seq: u64,
    root: [u8; HEAD - ROOT_START],
    size: u64,
}

struct SyncState {
    next: Mutex<Option<Round>>,
    wake: Condvar,
    done: AtomicU64,
}

/// One sync at a time runs on a second thread, so the event loop never waits for a disk.
pub struct Syncer {
    state: Arc<SyncState>,
}

fn fdatasync(fd: RawFd) {
    if unsafe { libc::fdatasync(fd) } != 0 {
        // After a failed fsync the kernel may have dropped the pages, so a retry can falsely
        // succeed. Nothing unsynced was acknowledged: exit, and a restart sees the last good
        // commit.
        eprintln!("fdatasync: {}", io::Error::last_os_error());
        std::process::exit(1);
    }
}

impl Syncer {
    pub fn start(fd: RawFd) -> Syncer {
        let state = Arc::new(SyncState { next: Mutex::new(None), wake: Condvar::new(), done: AtomicU64::new(0) });
        let s = state.clone();
        std::thread::Builder::new()
            .name("sync".into())
            .stack_size(64 * 1024)
            .spawn(move || {
                // End of the zeros written ahead of the commits. 0 after a start: whatever is
                // past the committed size then is written over.
                let mut ready = 0u64;
                loop {
                let round = {
                    let mut g = s.next.lock().unwrap();
                    loop {
                        match g.take() {
                            Some(r) => break r,
                            None => g = s.wake.wait(g).unwrap(),
                        }
                    }
                };
                // Nothing else writes the file during a round, so these can't hit a commit.
                if round.size + AHEAD / 2 > ready {
                    let from = ready.max(round.size);
                    ready = write_zeros(fd, from).map_or(ready, |()| from + AHEAD);
                }
                fdatasync(fd);
                if let Err(e) = pwrite_all(fd, &round.root, ROOT_START as u64) {
                    eprintln!("root header: {e}");
                    std::process::exit(1);
                }
                fdatasync(fd);
                s.done.store(round.seq, Ordering::Release);
                }
            })
            .expect("sync thread");
        Syncer { state }
    }

    /// Makes everything committed so far durable, with `head` as the root header.
    pub fn request(&self, seq: u64, head: &[u8; HEAD]) {
        let mut root = [0u8; HEAD - ROOT_START];
        root.copy_from_slice(&head[ROOT_START..]);
        *self.state.next.lock().unwrap() = Some(Round { seq, root, size: committed_size(head) });
        self.state.wake.notify_one();
    }

    #[inline]
    pub fn done(&self) -> u64 {
        self.state.done.load(Ordering::Acquire)
    }
}
