//! In-order index scanner. SQLite index B-trees store REAL entries in
//! interior cells (each interior cell is an actual (key,rowid) record), so
//! a scan must emit interior entries in order:
//! [left subtree][cell][next subtree] ... [rightmost subtree].
//!
//! State machine per level: InSubtree = inside the left subtree of `cell`;
//! AtCell = subtree done, `cell` is the next entry to emit; Done = popped.
//! The oracle test guards this ordering against SQLite itself; entries in
//! interior pages are the #1 way a naive leaf-only walk undercounts
//! (oracle catch, 2026-11-22).

use crate::page::{self, PageKind};
use crate::record::{decode_header, Field};
use crate::store::Store;
use crate::ReaderError;

const MAX_DEPTH: usize = 8;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Phase {
    /// the subtree under this level's current child pointer is in progress;
    /// when it pops back, this level emits its own cell, then enters the
    /// next subtree
    Child,
}

#[derive(Debug, Clone, Copy)]
struct Level {
    pgno: u32,
    ix: usize,
    ncells: usize,
    leaf: bool,
    /// right child for interior pages
    right: Option<u32>,
}

pub struct IndexScan<'a> {
    store: &'a Store,
    root: u32,
    stack: Vec<Level>,
}

impl<'a> IndexScan<'a> {
    fn page_bytes(&self, pgno: u32) -> Result<&'a [u8], ReaderError> {
        self.store.page(pgno)
    }

    #[allow(dead_code)]
    fn page(
        &self,
        pgno: u32,
    ) -> Result<(&'a [u8], PageKind, usize, usize, Option<u32>), ReaderError> {
        let bytes = self.page_bytes(pgno)?;
        let h = if pgno == 1 { 100 } else { 0 };
        let kind = match bytes.get(h).copied().unwrap_or(0) {
            page::PAGE_INTERIOR_INDEX => PageKind::InteriorIndex,
            page::PAGE_LEAF_INDEX => PageKind::LeafIndex,
            _ => return Err(ReaderError::Corrupt("index scan on non-index page")),
        };
        let ncells = be16(bytes, h + 3)?;
        let interior = kind == PageKind::InteriorIndex;
        let header_len = if interior { 12 } else { 8 };
        let right = if interior {
            Some(page::be32(bytes, h + 8).ok_or(ReaderError::OutOfRange)?)
        } else {
            None
        };
        Ok((bytes, kind, h + header_len, ncells, right))
    }

    pub fn new(store: &'a Store, root: u32) -> Result<Self, ReaderError> {
        Ok(Self { store, root, stack: Vec::with_capacity(MAX_DEPTH) })
    }

    fn push_level(&mut self, pgno: u32) -> Result<(), ReaderError> {
        if self.stack.len() >= MAX_DEPTH {
            return Err(ReaderError::Unsupported("btree too deep"));
        }
        let bytes = self.store.page(pgno)?;
        let h = if pgno == 1 { 100 } else { 0 };
        let kind_byte = *bytes.get(h).ok_or(ReaderError::OutOfRange)?;
        let (leaf, ncells, right) = match kind_byte {
            page::PAGE_LEAF_INDEX => (true, be16(bytes, h + 3)?, None),
            page::PAGE_INTERIOR_INDEX => {
                let right = page::be32(bytes, h + 8);
                (false, be16(bytes, h + 3)?, right)
            }
            _ => return Err(ReaderError::Corrupt("index scan on non-index page")),
        };
        self.stack.push(Level { pgno, ix: 0, ncells, leaf, right });
        Ok(())
    }

    /// (child-if-interior, payload) for cell `cell` of page `pgno`.
    fn cell_entry(
        &self,
        level: &Level,
        cell: usize,
    ) -> Result<(Option<u32>, &'a [u8]), ReaderError> {
        let bytes = self.store.page(level.pgno)?;
        if cell >= level.ncells {
            return Err(ReaderError::OutOfRange);
        }
        let off = {
            let p = level_pg_ptr(bytes, level.pgno) + cell * 2;
            let o = page::be16(bytes, p).ok_or(ReaderError::OutOfRange)? as usize;
            if o >= bytes.len() {
                return Err(ReaderError::Corrupt("cell offset out of page"));
            }
            o
        };
        let interior = !level.leaf;
        let mut p = off;
        let child = if interior {
            let c = page::be32(bytes, p).ok_or(ReaderError::OutOfRange)?;
            p += 4;
            Some(c)
        } else {
            None
        };
        let (plen, vn) =
            crate::varint::decode(&bytes[p..]).map_err(|_| ReaderError::Corrupt("cell payload len"))?;
        let start = p + vn;
        let usable = bytes.len();
        let max_local = ((usable - 12) * 64 / 255) - 23;
        if plen as usize > max_local {
            return Err(ReaderError::Unsupported("overflow index cell"));
        }
        let end = start + plen as usize;
        if end > bytes.len() {
            return Err(ReaderError::Corrupt("index cell payload past page"));
        }
        Ok((child, &bytes[start..end]))
    }

    fn key_of(payload: &[u8]) -> Result<i64, ReaderError> {
        let mut f = [Field { serial_type: 0, offset: 0, len: 0 }; 4];
        let (_, n) = decode_header(payload, &mut f).map_err(|_| ReaderError::Corrupt("index record"))?;
        if n == 0 {
            return Err(ReaderError::Corrupt("empty index record"));
        }
        let b = &payload[f[0].offset..f[0].offset + f[0].len];
        Ok(match f[0].serial_type {
            1 => b[0] as i8 as i64,
            2 => i16::from_be_bytes([b[0], b[1]]) as i64,
            3 => ((b[0] as i64) << 16) | ((b[1] as i64) << 8) | (b[2] as i64),
            4 => i32::from_be_bytes([b[0], b[1], b[2], b[3]]) as i64,
            5 => {
                let mut v = ((b[0] as i64) << 40)
                    | ((b[1] as i64) << 32)
                    | ((b[2] as i64) << 24)
                    | ((b[3] as i64) << 16)
                    | ((b[4] as i64) << 8)
                    | (b[5] as i64);
                if b[0] & 0x80 != 0 {
                    v |= !0i64 << 48;
                }
                v
            }
            6 => i64::from_be_bytes(b.try_into().map_err(|_| ReaderError::Corrupt("int6"))?),
            8 => 0,
            9 => 1,
            _ => return Err(ReaderError::Corrupt("non-integer index key")),
        })
    }

    /// Position at the first in-order entry whose first field is >= `key`.
    pub fn lower_bound(&mut self, key: i64) -> Result<(), ReaderError> {
        self.stack.clear();
        let mut pgno = self.root;
        loop {
            if self.stack.len() >= MAX_DEPTH {
                return Err(ReaderError::Unsupported("btree too deep"));
            }
            let (bytes, kind, ptr_array, ncells, right) = self.page(pgno)?;
            let interior = kind == PageKind::InteriorIndex;
            if !interior {
                let mut chosen = ncells; // past end => EOF
                for i in 0..ncells {
                    let p = ptr_array + i * 2;
                    let off = page::be16(bytes, p).ok_or(ReaderError::OutOfRange)? as usize;
                    let (_, payload) = self.cell_entry_raw(bytes, off, false)?;
                    if Self::key_of(payload)? >= key {
                        chosen = i;
                        break;
                    }
                }
                let leaf = true;
                let right = None;
                self.stack.push(Level { pgno, ix: chosen, ncells, leaf, right });
                return Ok(());
            }
            let header_len = 12;
            let mut chosen = ncells;
            let mut child = right.ok_or(ReaderError::Corrupt("no right child"))?;
            for i in 0..ncells {
                let p = ptr_array + i * 2;
                let off = page::be16(bytes, p).ok_or(ReaderError::OutOfRange)? as usize;
                let (c, payload) = self.cell_entry_raw(bytes, off, true)?;
                if Self::key_of(payload)? >= key {
                    chosen = i;
                    child = c.ok_or(ReaderError::Corrupt("interior cell without child"))?;
                    break;
                }
            }
            let right_opt = Some(right.ok_or(ReaderError::Corrupt("no right child"))?);
            self.stack.push(Level { pgno, ix: chosen, ncells, leaf: false, right: right_opt });
            pgno = child;
        }
    }

    /// Raw cell parse against already-fetched page bytes.
    fn cell_entry_raw<'b>(
        &self,
        bytes: &'b [u8],
        off: usize,
        interior: bool,
    ) -> Result<(Option<u32>, &'b [u8]), ReaderError> {
        let mut p = off;
        let child = if interior {
            let c = page::be32(bytes, p).ok_or(ReaderError::OutOfRange)?;
            p += 4;
            Some(c)
        } else {
            None
        };
        let (plen, vn) =
            crate::varint::decode(&bytes[p..]).map_err(|_| ReaderError::Corrupt("cell payload len"))?;
        let start = p + vn;
        let usable = bytes.len();
        let max_local = ((usable - 12) * 64 / 255) - 23;
        if plen as usize > max_local {
            return Err(ReaderError::Unsupported("overflow index cell"));
        }
        let end = start + plen as usize;
        if end > bytes.len() {
            return Err(ReaderError::Corrupt("index cell payload past page"));
        }
        Ok((child, &bytes[start..end]))
    }

    fn level_payload(&self, level: &Level, cell: usize) -> Result<&'a [u8], ReaderError> {
        let (_, payload) = self.cell_entry(level, cell)?;
        Ok(payload)
    }

    /// In-order advance. Port of SQLite btreeNext (sqlite3.c 3.54). Invariant:
    /// every level's `ix` is the index of the NEXT entry to emit at that
    /// level; the loop top increments before reading, so emission uses the
    /// pre-increment copy. Interior cells are real entries, emitted exactly
    /// once on the pop-back of their left subtree.
    pub fn advance(&mut self) -> Result<Option<&'a [u8]>, ReaderError> {
        loop {
            let Some(top_m) = self.stack.last_mut() else {
                return Ok(None);
            };
            let top = *top_m;
            top_m.ix += 1;
            if top.ix < top.ncells {
                if top.leaf {
                    return Ok(Some(self.level_payload(&top, top.ix)?));
                }
                // interior: next entry = leftmost of child_of(ix)
                let (child, _) = self.cell_entry(&top, top.ix)?;
                let child = child.ok_or(ReaderError::Corrupt("interior cell without child"))?;
                if let Some(p) = self.descend_leftmost_emit(child)? {
                    return Ok(Some(p));
                }
                continue;
            }
            // ix == ncells: subtree exhausted
            if !top.leaf {
                // past the last cell of an interior page: descend right
                let right = top.right.ok_or(ReaderError::Corrupt("no right child"))?;
                if let Some(p) = self.descend_leftmost_emit(right)? {
                    return Ok(Some(p));
                }
                continue;
            }
            // Leaf exhausted -> btreeNext's pop loop: keep moving UP while
            // each ancestor is itself exhausted (ix >= ncells: its cells and
            // right subtree are all done). The first live ancestor emits its
            // separator cell at its ix, then ix advances past it so the next
            // call enters child_of(ix+1). Never re-descend a right subtree.
            loop {
                self.stack.pop();
                let Some(parent) = self.stack.last().copied() else {
                    return Ok(None);
                };
                if !parent.leaf && parent.ix < parent.ncells {
                    if let Some(m) = self.stack.last_mut() {
                        m.ix += 1;
                    }
                    return Ok(Some(self.level_payload(&parent, parent.ix)?));
                }
            }
        }
    }

    /// Push levels down to the leftmost leaf under `pgno` and emit its first
    /// cell. An empty leaf (only possible for an empty root) yields None.
    fn descend_leftmost_emit(
        &mut self,
        mut pgno: u32,
    ) -> Result<Option<&'a [u8]>, ReaderError> {
        loop {
            let bytes = self.page_bytes(pgno)?;
            let h = if pgno == 1 { 100 } else { 0 };
            let kind_byte = *bytes.get(h).ok_or(ReaderError::OutOfRange)?;
            let ncells = be16(bytes, h + 3)?;
            let interior = kind_byte == page::PAGE_INTERIOR_INDEX;
            if interior {
                let off = {
                    let p = h + 12;
                    page::be16(bytes, p).ok_or(ReaderError::OutOfRange)? as usize
                };
                let (child, _) = self.cell_entry_raw(bytes, off, true)?;
                let child = child.ok_or(ReaderError::Corrupt("interior cell without child"))?;
                self.push_level(pgno)?;
                pgno = child;
                continue;
            }
            if ncells == 0 {
                return Ok(None);
            }
            let leaf = true;
            let right = None;
            // cell[0] is returned as this call's entry; ix starts at 1 so the
            // next advance() emits cell[1] (invariant: ix = next-to-emit).
            self.stack.push(Level { pgno, ix: 1, ncells, leaf, right });
            let lvl = self.stack.last().unwrap();
            return Ok(Some(self.level_payload(lvl, 0)?));
        }
    }

    /// Public key decode (oracle diagnostics).
    pub fn key_of_pub(payload: &[u8]) -> i64 {
        Self::key_of(payload).unwrap_or(i64::MIN)
    }

    /// Public (key, rowid) decode for index records (oracle diagnostics).
    pub fn key_row_pub(payload: &[u8]) -> (i64, i64) {
        let mut f = [Field { serial_type: 0, offset: 0, len: 0 }; 8];
        let (_, n) = decode_header(payload, &mut f).unwrap_or((0, 0));
        let mut vals = [0i64; 8];
        for (i, fi) in f.iter().take(n).enumerate() {
            let b = &payload[fi.offset..fi.offset + fi.len];
            vals[i] = match fi.serial_type {
                1 => b[0] as i8 as i64,
                2 => i16::from_be_bytes([b[0], b[1]]) as i64,
                3 => ((b[0] as i64) << 16) | ((b[1] as i64) << 8) | (b[2] as i64),
                4 => i32::from_be_bytes([b[0], b[1], b[2], b[3]]) as i64,
                8 => 0,
                9 => 1,
                _ => i64::MIN,
            };
        }
        (vals[0], vals[1])
    }

    /// Count entries whose first field equals `key` (lower_bound + walk).
    pub fn count_prefix(&mut self, key: i64) -> Result<i64, ReaderError> {
        self.lower_bound(key)?;
        let mut count = 0i64;
        while let Some(payload) = self.advance()? {
            let k = Self::key_of(payload)?;
            if k != key {
                break;
            }
            count += 1;
        }
        Ok(count)
    }
}

fn be16(b: &[u8], off: usize) -> Result<usize, ReaderError> {
    let v = page::be16(b, off).ok_or(ReaderError::OutOfRange)? as usize;
    Ok(v)
}

/// Cell pointer array start for a page of this number (page 1 has the
/// 100-byte database header first). Header length is 12 for interior
/// pages, 8 for leaves.
fn level_pg_ptr(bytes: &[u8], pgno: u32) -> usize {
    let h = if pgno == 1 { 100 } else { 0 };
    let kind = bytes[h];
    h + if kind == page::PAGE_INTERIOR_INDEX || kind == page::PAGE_INTERIOR_TABLE { 12 } else { 8 }
}
