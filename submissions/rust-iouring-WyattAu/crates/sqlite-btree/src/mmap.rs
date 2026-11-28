//! Read-only mmap wrapper. The single unsafe surface of the reader:
//! mapping lifetime vs file growth. Pattern per estate `shm-rings`
//! THREAT-MODEL: inherent-unsafe surface, documented and scoped.

use memmap2::Mmap;
use std::fs::File;
use std::io;
use std::path::Path;

/// A read-only memory map that can follow file growth.
///
/// Safety contract: the mapping is read-only and the backing file is opened
/// read-only; the OS may evict pages at any time (they are re-read on
/// access), so no stable addresses are promised across [`Self::remap_if_grown`]
/// calls. Callers must not hold slices across a remap.
pub struct MappedFile {
    file: File,
    mmap: Mmap,
}

impl std::fmt::Debug for MappedFile {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("MappedFile").field("len", &self.mmap.len()).finish()
    }
}

impl MappedFile {
    pub fn open(path: impl AsRef<Path>) -> io::Result<Self> {
        let file = File::open(path)?;
        let mmap = unsafe { memmap2::Mmap::map(&file)? };
        Ok(Self { file, mmap })
    }

    /// Map `path` with the given base offset already applied upstream.
    pub fn len(&self) -> usize {
        self.mmap.len()
    }

    pub fn is_empty(&self) -> bool {
        self.mmap.is_empty()
    }

    pub fn as_slice(&self) -> &[u8] {
        &self.mmap[..]
    }

    /// Re-map if the file grew (SQLite extends the database at checkpoints
    /// and appends to the WAL on commit). Returns true when a remap happened.
    pub fn remap_if_grown(&mut self) -> io::Result<bool> {
        let on_disk = self.file.metadata()?.len() as usize;
        if on_disk > self.mmap.len() {
            self.mmap = unsafe { memmap2::Mmap::map(&self.file)? };
            Ok(true)
        } else {
            Ok(false)
        }
    }
}
