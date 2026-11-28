//! WAL frame index: maps page number -> latest frame at or before a snapshot.
//! Protocol per ADR 0004: we are the only writer, so frames append
//! monotonically between checkpoints; the index extends incrementally and
//! re-bases when a checkpoint truncates the WAL (salts change, file shrinks).
//!
//! Checksum chain (fileformat2.html section 4.2.2): seeded by the checksum
//! over the WAL header's first 24 bytes; each frame contributes its first
//! 8 header bytes plus the page content, in u32 pairs read at the byte order
//! chosen by the magic number's low bit (0x82 = little-endian words).

use crate::{ReaderError, mmap::MappedFile};
use std::collections::HashMap;
use std::fs::File;
use std::io;
use std::path::Path;

const WAL_HEADER: usize = 32;
const FRAME_HEADER: usize = 24;
const MAGIC_LE: u32 = 0x377f0682; // checksum words read little-endian
const MAGIC_BE: u32 = 0x377f0683; // big-endian words

#[derive(Debug)]
pub struct WalIndex {
    pub page_size: usize,
    salt1: u32,
    salt2: u32,
    big_endian: bool,
    /// pgno -> latest frame index (frames are append-only until truncation).
    /// NOTE: `latest_frame` treats any snapshot as "all frames"; correct for
    /// the single-threaded pass model (ADR 0003) where snapshots are always
    /// refreshed to the current end before reads.
    latest: HashMap<u32, u32>,
    /// database size in pages per the most recent commit frame
    pub committed_db_pages: u32,
    /// running checksum chain after the last indexed frame
    chain: (u32, u32),
    mapped_len: usize,
}

impl WalIndex {
    /// Open and fully parse the WAL at `path`. A missing (or zero-length)
    /// file is `Ok(Ok(None))`: a database with no WAL is an empty snapshot.
    pub fn open(
        path: impl AsRef<Path>,
        expected_page_size: usize,
    ) -> io::Result<Result<Option<Self>, ReaderError>> {
        let file = match File::open(path.as_ref()) {
            Ok(f) => f,
            Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(Ok(None)),
            Err(e) => return Err(e),
        };
        let map = unsafe { memmap2::Mmap::map(&file)? };
        if map.len() == 0 {
            return Ok(Ok(None));
        }
        if map.len() < WAL_HEADER {
            return Ok(Err(ReaderError::WalHeaderBad));
        }
        let magic = be32(&map, 0);
        let big_endian = match magic {
            MAGIC_BE => true,
            MAGIC_LE => false,
            _ => return Ok(Err(ReaderError::WalHeaderBad)),
        };
        let page_size = be32(&map, 8) as usize;
        if page_size != expected_page_size {
            return Ok(Err(ReaderError::WalHeaderBad));
        }
        let salt1 = be32(&map, 16);
        let salt2 = be32(&map, 20);
        // header checksum over its first 24 bytes, seeded (0, 0)
        let seed = wal_checksum(&map[0..24], (0, 0), big_endian);
        let stored = (be32(&map, 24), be32(&map, 28));
        if seed != stored {
            return Ok(Err(ReaderError::WalHeaderBad));
        }

        let mut idx = WalIndex {
            page_size,
            salt1,
            salt2,
            big_endian,
            latest: HashMap::new(),
            committed_db_pages: 0,
            chain: seed,
            // scan starts after the header; full frame chain verified at open
            mapped_len: WAL_HEADER,
        };
        // SQLite semantics: the first invalid frame TERMINATES the wal.
        // After a RESTART checkpoint the file keeps old-generation bytes
        // beyond the new frames; those fail salt/chain checks and are
        // simply not indexed (the valid prefix is the database).
        match idx.scan_new_frames(&map[..]) {
            Ok(ScanResult::Invalid) | Ok(ScanResult::Truncated) => {
                idx.mapped_len = map.len();
            }
            Ok(_) => {}
            Err(e) => return Err(io::Error::new(io::ErrorKind::Other, e.to_string())),
        }
        Ok(Ok(Some(idx)))
    }

    #[inline]
    pub fn frame_stride(&self) -> usize {
        FRAME_HEADER + self.page_size
    }

    /// Current header salts: a mismatch at refresh time means SQLite
    /// checkpoint-restarted the WAL underneath us (cache spill does this
    /// even with autocheckpoint=0); the index must re-open from frame 0.
    pub fn header_salts(map: &MappedFile) -> Option<(u32, u32)> {
        let s = map.as_slice();
        if s.len() < 24 {
            return None;
        }
        Some((be32(s, 16), be32(s, 20)))
    }

    #[inline]
    pub fn salts(&self) -> (u32, u32) {
        (self.salt1, self.salt2)
    }

    pub fn mapped_len_dbg(&self) -> usize {
        self.mapped_len
    }

    pub fn frame_count(&self) -> u32 {
        self.latest.values().copied().max().map_or(0, |f| f + 1)
    }

    /// Latest frame index for `pgno` at or before `snapshot`.
    #[inline]
    pub fn latest_frame(&self, pgno: u32, snapshot: u32) -> Option<u32> {
        let f = self.latest.get(&pgno).copied()?;
        (f < snapshot).then_some(f)
    }

    /// Page content bytes for frame `f` inside the mapped WAL.
    pub fn frame_page<'a>(&self, wal: &'a MappedFile, f: u32) -> &'a [u8] {
        let off = WAL_HEADER + f as usize * self.frame_stride() + FRAME_HEADER;
        let s = wal.as_slice();
        &s[off..off + self.page_size]
    }

    /// Extend the index over appended frames. `Truncated` means the file
    /// shrank (checkpoint): the caller must `re_open`. `Invalid` means a
    /// salt/checksum mismatch: caller falls back.
    pub fn scan_new_frames(&mut self, wal: &[u8]) -> Result<ScanResult, ReaderError> {
        if wal.len() == self.mapped_len {
            return Ok(ScanResult::NoChange);
        }
        if wal.len() < self.mapped_len {
            return Ok(ScanResult::Truncated);
        }
        if (wal.len() - WAL_HEADER) % self.frame_stride() != 0 {
            return Ok(ScanResult::Invalid);
        }
        let n_frames = (wal.len() - WAL_HEADER) / self.frame_stride();
        for f in self.frame_count() as usize..n_frames {
            let base = WAL_HEADER + f * self.frame_stride();
            if base + FRAME_HEADER > wal.len() {
                return Ok(ScanResult::Invalid);
            }
            let pgno = be32(wal, base);
            let commit_db_pages = be32(wal, base + 4);
            let fs1 = be32(wal, base + 8);
            let fs2 = be32(wal, base + 12);
            if fs1 != self.salt1 || fs2 != self.salt2 {
                eprintln!("scan: salt mismatch at frame {f}: got ({fs1:#x},{fs2:#x}) want ({:#x},{:#x})", self.salt1, self.salt2);
                return Ok(ScanResult::Invalid);
            }
            if commit_db_pages != 0 {
                self.committed_db_pages = commit_db_pages;
            }
            // chain: frame header's first 8 bytes, then the page content
            let mut c = self.chain;
            c = wal_checksum(&wal[base..base + 8], c, self.big_endian);
            let page_start = base + FRAME_HEADER;
            let page_end = page_start + self.page_size;
            c = wal_checksum(&wal[page_start..page_end], c, self.big_endian);
            if (be32(wal, base + 16), be32(wal, base + 20)) != c {
                eprintln!("scan: checksum mismatch at frame {f}");
                return Ok(ScanResult::Invalid);
            }
            self.chain = c;
            self.latest.insert(pgno, f as u32);
        }
        self.mapped_len = wal.len();
        Ok(ScanResult::Extended)
    }

    /// Re-base after a checkpoint truncated the WAL.
    pub fn re_open(
        path: impl AsRef<Path>,
        expected_page_size: usize,
    ) -> io::Result<Result<Option<Self>, ReaderError>> {
        Self::open(path, expected_page_size)
    }
}

pub enum ScanResult {
    NoChange,
    Extended,
    Truncated,
    Invalid,
}

#[inline]
fn be32(b: &[u8], off: usize) -> u32 {
    u32::from_be_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

/// SQLite WAL checksum: u32 pairs, word order per the magic's low bit.
fn wal_checksum(bytes: &[u8], (mut s1, mut s2): (u32, u32), big_endian: bool) -> (u32, u32) {
    for pair in bytes.chunks_exact(8) {
        let x1 = if big_endian {
            u32::from_be_bytes([pair[0], pair[1], pair[2], pair[3]])
        } else {
            u32::from_le_bytes([pair[0], pair[1], pair[2], pair[3]])
        };
        let x2 = if big_endian {
            u32::from_be_bytes([pair[4], pair[5], pair[6], pair[7]])
        } else {
            u32::from_le_bytes([pair[4], pair[5], pair[6], pair[7]])
        };
        s1 = s1.wrapping_add(x1).wrapping_add(s2);
        s2 = s2.wrapping_add(x2).wrapping_add(s1);
    }
    (s1, s2)
}
