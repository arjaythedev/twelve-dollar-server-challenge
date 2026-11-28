//! PageStore: unified read view over the database file and its WAL at a
//! chosen snapshot (ADR 0004). `page(n)` returns the newest committed bytes
//! for page `n` at the store's snapshot; the caller advances the snapshot
//! after write batches commit.
//!
//! Refresh rules:
//! - remap db/WAL when either grows (remap_if_grown)
//! - a WAL header salt change means SQLite checkpoint-restarted underneath
//!   us (cache spill does this even with autocheckpoint=0): re-open the
//!   index from frame 0
//! - a shrunken WAL means a TRUNCATE checkpoint: re-open as well

use crate::mmap::MappedFile;
use crate::wal::{ScanResult, WalIndex};
use crate::ReaderError;
use std::path::{Path, PathBuf};

#[derive(Debug)]
pub struct Store {
    db: MappedFile,
    db_path: PathBuf,
    wal: Option<(MappedFile, WalIndex)>,
    page_size: usize,
    snapshot: u32,
    db_pages: usize,
}

fn wal_path(db_path: &Path) -> PathBuf {
    let mut p = db_path.to_path_buf().into_os_string();
    p.push("-wal");
    PathBuf::from(p)
}

fn io_err(_e: std::io::Error) -> ReaderError {
    ReaderError::Corrupt("io error")
}

#[inline]
fn be32(b: &[u8], off: usize) -> u32 {
    u32::from_be_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

impl Store {
    /// Open the database file (and its WAL, when present). Validates the
    /// header invariants our reader relies on: power-of-two page size,
    /// UTF-8 encoding (0/1; 0 is a fresh, not-yet-finalized file), zero
    /// reserved bytes per page.
    pub fn open(db_path: impl AsRef<Path>) -> Result<Self, ReaderError> {
        let db_path = db_path.as_ref().to_path_buf();
        let db = MappedFile::open(&db_path).map_err(io_err)?;
        let s = db.as_slice();
        if s.len() < 100 {
            return Err(ReaderError::Corrupt("db header truncated"));
        }
        let raw_ps = u16::from_be_bytes([s[16], s[17]]) as usize;
        let page_size = if raw_ps == 1 { 65536 } else { raw_ps };
        if page_size < 512 || (page_size & (page_size - 1)) != 0 {
            return Err(ReaderError::Unsupported("non power-of-two page size"));
        }
        if s[20] != 0 {
            return Err(ReaderError::Unsupported("reserved bytes per page"));
        }
        // Encoding: 1 = UTF-8. A freshly initialized file may carry 0 (unset);
        // SQLite defaults to UTF-8. 2/3 (UTF-16) are Unsupported.
        let enc = u32::from_be_bytes([s[56], s[57], s[58], s[59]]);
        if enc != 0 && enc != 1 {
            return Err(ReaderError::Unsupported("text encoding not UTF-8"));
        }
        if s.len() % page_size != 0 {
            return Err(ReaderError::Corrupt("db size not page-aligned"));
        }
        let db_pages = s.len() / page_size;

        let wp = wal_path(&db_path);
        let wal = match WalIndex::open(&wp, page_size).map_err(io_err)? {
            Ok(Some(idx)) => {
                let map = MappedFile::open(&wp).map_err(io_err)?;
                Some((map, idx))
            }
            Ok(None) => None,
            Err(e) => return Err(e),
        };
        let snapshot = wal.as_ref().map_or(0, |(_, idx)| idx.frame_count());
        Ok(Self { db, db_path, wal, page_size, snapshot, db_pages })
    }

    pub fn page_size(&self) -> usize {
        self.page_size
    }

    pub fn snapshot(&self) -> u32 {
        self.snapshot
    }

    /// Advance the snapshot to the WAL's current end (after write batches
    /// committed through the writer connection).
    pub fn refresh(&mut self) -> Result<(), ReaderError> {
        self.db.remap_if_grown().map_err(io_err)?;
        if self.db.as_slice().len() % self.page_size != 0 {
            return Err(ReaderError::Corrupt("db size not page-aligned after growth"));
        }
        self.db_pages = self.db.as_slice().len() / self.page_size;

        let wp = wal_path(&self.db_path);

        // decide the WAL action without holding a borrow across reassignment
        enum Action {
            Scan,
            Reopen,
        }
        let action = match &mut self.wal {
            None => {
                // a WAL may have appeared (first write)
                match WalIndex::open(&wp, self.page_size).map_err(io_err)? {
                    Ok(Some(_)) => Action::Reopen,
                    Ok(None) => {
                        self.snapshot = 0;
                        return Ok(());
                    }
                    Err(e) => return Err(e),
                }
            }
            Some((map, idx)) => {
                // size on disk first: growth remaps in place; SHRINK (a
                // TRUNCATE checkpoint) invalidates every byte of the old
                // mapping — touching it is SIGBUS, so re-open instead
                let on_disk = std::fs::metadata(&wp).map(|m| m.len() as usize).unwrap_or(0);
                if on_disk < map.len() {
                    Action::Reopen
                } else {
                    map.remap_if_grown().map_err(io_err)?;
                    // checkpoint-restart rewrites frames from slot 0 with new
                    // salts: any index we hold is invalid the moment salts drift
                    let restarted =
                        WalIndex::header_salts(map).is_none_or(|salts| salts != idx.salts());
                    if restarted {
                        Action::Reopen
                    } else {
                        match idx.scan_new_frames(map.as_slice())? {
                            ScanResult::NoChange | ScanResult::Extended => Action::Scan,
                            ScanResult::Truncated | ScanResult::Invalid => Action::Reopen,
                        }
                    }
                }
            }
        };

        match action {
            Action::Scan => {
                self.snapshot = self.wal.as_ref().map_or(0, |(_, idx)| idx.frame_count());
            }
            Action::Reopen => {
                match WalIndex::open(&wp, self.page_size).map_err(io_err)? {
                    Ok(Some(idx)) => {
                        let map = MappedFile::open(&wp).map_err(io_err)?;
                        self.snapshot = idx.frame_count();
                        self.wal = Some((map, idx));
                    }
                    Ok(None) => {
                        self.wal = None;
                        self.snapshot = 0;
                    }
                    Err(e) => return Err(e),
                }
            }
        }
        Ok(())
    }

    /// Page bytes at the current snapshot. Pages are 1-based.
    pub fn page(&self, pgno: u32) -> Result<&[u8], ReaderError> {
        if pgno == 0 {
            return Err(ReaderError::OutOfRange);
        }
        if let Some((map, idx)) = &self.wal {
            if let Some(f) = idx.latest_frame(pgno, self.snapshot) {
                return Ok(idx.frame_page(map, f));
            }
        }
        // logical size: db file pages plus pages that exist only in the WAL
        let logical = self.wal.as_ref().map_or(self.db_pages, |(_, idx)| {
            self.db_pages.max(idx.committed_db_pages as usize)
        });
        let n = pgno as usize;
        if n > logical {
            return Err(ReaderError::OutOfRange);
        }
        if n <= self.db_pages {
            let s = self.db.as_slice();
            return Ok(&s[(n - 1) * self.page_size..n * self.page_size]);
        }
        // page exists logically but has no frame at this snapshot and is not
        // checkpointed: unreachable for valid trees at a refreshed snapshot
        Err(ReaderError::OutOfRange)
    }
}
