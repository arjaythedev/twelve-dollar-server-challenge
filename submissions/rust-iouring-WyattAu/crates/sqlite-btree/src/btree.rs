//! B-tree cursor over the page store: table seek/rightmost/backward walk and
//! index lower-bound/forward walk with a bounded cursor stack (height bound
//! is a Lean proof target; stack is a fixed inline array of depth 8).
//!
//! Scope note: this cursor serves the challenge's four read patterns only.
//! Overflow cells return `Unsupported` (server falls back per REQ-URIK-R5).

use crate::page::{self, PageHeader, PageKind};
use crate::store::Store;
use crate::ReaderError;

/// Max btree depth: 12 is SQLite's own soft ceiling; 8 covers any realistic
/// tree of our fanout (Lean target: height(btree) <= 8 for our schema).
pub const MAX_DEPTH: usize = 8;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Step {
    /// moved to a valid entry
    Moved,
    /// no more entries in that direction
    End,
}

#[derive(Debug, Clone, Copy)]
struct Level {
    pgno: u32,
    cell: usize,
    header: PageHeader,
}

#[derive(Debug)]
pub struct Cursor<'a> {
    store: &'a Store,
    root: u32,
    stack: Vec<Level>,
    /// table cursor vs index cursor changes cell decoding
    index: bool,
}

impl<'a> Cursor<'a> {
    pub fn table(store: &'a Store, root: u32) -> Result<Self, ReaderError> {
        Ok(Self { store, root, stack: Vec::with_capacity(8), index: false })
    }

    pub fn index(store: &'a Store, root: u32) -> Result<Self, ReaderError> {
        Ok(Self { store, root, stack: Vec::with_capacity(8), index: true })
    }

    fn page(&self, pgno: u32) -> Result<(&'a [u8], PageHeader), ReaderError> {
        let bytes = self.store.page(pgno)?;
        let header = page::parse_page(bytes, pgno)?;
        Ok((bytes, header))
    }

    fn push_level(&mut self, pgno: u32) -> Result<(), ReaderError> {
        if self.stack.len() >= MAX_DEPTH {
            return Err(ReaderError::Unsupported("btree too deep"));
        }
        let (bytes, header) = self.page(pgno)?;
        self.stack.push(Level { pgno, cell: 0, header });
        let _ = bytes;
        Ok(())
    }

    fn top(&self) -> Result<&Level, ReaderError> {
        self.stack.last().ok_or(ReaderError::Corrupt("empty cursor stack"))
    }

    fn top_mut(&mut self) -> Result<&mut Level, ReaderError> {
        self.stack.last_mut().ok_or(ReaderError::Corrupt("empty cursor stack"))
    }

    /// Descend from the current stack to the leftmost leaf under the child
    /// pointer at the top's current cell (or the right pointer if past end).
    fn descend(&mut self, child: u32) -> Result<(), ReaderError> {
        let mut pgno = child;
        loop {
            self.push_level(pgno)?;
            let top = *self.top()?;
            let (bytes, _) = self.page(top.pgno)?;
            if !top.header.kind.is_interior() {
                return Ok(());
            }
            pgno = if top.cell < top.header.ncells {
                let off = page::cell_offset(bytes, &top.header, top.cell)?;
                if self.index {
                    page::parse_index_cell(bytes, off, true)?.0
                } else {
                    page::parse_table_interior_cell(bytes, off)?.0
                }
            } else {
                top.header.right_child.ok_or(ReaderError::Corrupt("no right child"))?
            };
        }
    }

    /// Move to the first entry of the tree.
    pub fn first(&mut self) -> Result<Step, ReaderError> {
        self.stack.clear();
        self.descend(self.root)?;
        Ok(Step::Moved)
    }

    /// Move to the last entry of the tree (right spine, last cell).
    /// Interior levels are marked cell = ncells ("in the right subtree") so
    /// backward stepping pops correctly across page boundaries.
    pub fn last(&mut self) -> Result<Step, ReaderError> {
        self.stack.clear();
        let mut pgno = self.root;
        loop {
            self.push_level(pgno)?;
            let top = *self.top()?;
            let (bytes, _) = self.page(top.pgno)?;
            if !top.header.kind.is_interior() {
                let n = top.header.ncells;
                if n == 0 {
                    return Ok(Step::End);
                }
                self.top_mut()?.cell = n - 1;
                return Ok(Step::Moved);
            }
            self.top_mut()?.cell = top.header.ncells;
            pgno = top.header.right_child.ok_or(ReaderError::Corrupt("no right child"))?;
        }
    }

    /// Step forward. Crosses pages via the stack (pop, step cell, descend
    /// left subtree's rightmost path when moving into an interior level).
    pub fn next(&mut self) -> Result<Step, ReaderError> {
        self.step_dir(false)
    }

    /// Step backward.
    pub fn prev(&mut self) -> Result<Step, ReaderError> {
        self.step_dir(true)
    }

    fn step_dir(&mut self, backward: bool) -> Result<Step, ReaderError> {
        loop {
            {
                let top = *self.top()?;
                let next_idx = if backward { top.cell.wrapping_sub(1) } else { top.cell + 1 };
                let has = if backward {
                    next_idx != usize::MAX
                } else {
                    next_idx < top.header.ncells
                };
                if has {
                    self.top_mut()?.cell = next_idx;
                    return Ok(Step::Moved);
                }
            }
            // pop and try the parent level
            self.stack.pop();
            match self.stack.last_mut() {
                None => return Ok(Step::End),
                Some(parent) => {
                    if backward {
                        if parent.cell == 0 {
                            // nothing before this subtree at the parent: keep popping
                            continue;
                        }
                        parent.cell -= 1;
                    } else {
                        parent.cell += 1;
                    }
                    let parent_snapshot = *parent;
                    let (bytes, _) = self.page(parent_snapshot.pgno)?;
                    if parent_snapshot.header.kind.is_interior() {
                        let child = if parent_snapshot.cell < parent_snapshot.header.ncells {
                            let off = page::cell_offset(bytes, &parent_snapshot.header, parent_snapshot.cell)?;
                            if self.index {
                                page::parse_index_cell(bytes, off, true)?.0
                            } else {
                                page::parse_table_interior_cell(bytes, off)?.0
                            }
                        } else {
                            parent_snapshot.header.right_child.ok_or(ReaderError::Corrupt("no right child"))?
                        };
                        // descend into the child subtree, landing on its
                        // far-side leaf (rightmost when moving backward)
                        let mut pgno = child;
                        loop {
                            self.push_level(pgno)?;
                            let top = *self.top()?;
                            let (bytes, _) = self.page(top.pgno)?;
                            if top.header.kind.is_interior() {
                                pgno = if backward {
                                    // descend toward the subtree's last entry
                                    let last_cell = top.header.ncells.saturating_sub(1);
                                    self.top_mut()?.cell = last_cell;
                                    let off = page::cell_offset(bytes, &top.header, last_cell)?;
                                    page::parse_index_cell(bytes, off, true)?.0
                                } else {
                                    top.header.right_child.ok_or(ReaderError::Corrupt("no right child"))?
                                };
                            } else {
                                let n = top.header.ncells;
                                self.top_mut()?.cell = if backward { n.saturating_sub(1) } else { 0 };
                                return Ok(Step::Moved);
                            }
                        }
                    } else {
                        continue;
                    }
                }
            }
        }
    }

    /// Table seek by rowid: positions at the entry if present (returns true),
    /// else at the first entry >= rowid (returns false); End on empty tree.
    pub fn table_seek(&mut self, rowid: i64) -> Result<TableSeek, ReaderError> {
        self.stack.clear();
        let mut pgno = self.root;
        loop {
            self.push_level(pgno)?;
            let top = *self.top()?;
            let (bytes, header) = self.page(top.pgno)?;
            if header.kind.is_interior() {
                // binary search interior cells by key; descend the left child
                // of the first cell with key >= rowid, else the right pointer
                let mut lo = 0usize;
                let mut hi = header.ncells;
                let mut child = header.right_child.ok_or(ReaderError::Corrupt("no right child"))?;
                while lo < hi {
                    let mid = (lo + hi) / 2;
                    let off = page::cell_offset(bytes, &header, mid)?;
                    let (c, key) = page::parse_table_interior_cell(bytes, off)?;
                    if key < rowid {
                        lo = mid + 1;
                    } else {
                        hi = mid;
                        child = c;
                    }
                }
                self.top_mut()?.cell = lo;
                pgno = child;
            } else {
                // leaf: binary search cells by rowid
                let mut lo = 0usize;
                let mut hi = header.ncells;
                while lo < hi {
                    let mid = (lo + hi) / 2;
                    let off = page::cell_offset(bytes, &header, mid)?;
                    let cell = page::parse_table_leaf_cell(bytes, &header, off)?;
                    if cell.rowid < rowid {
                        lo = mid + 1;
                    } else {
                        hi = mid;
                    }
                }
                self.top_mut()?.cell = lo;
                if lo < header.ncells {
                    let off = page::cell_offset(bytes, &header, lo)?;
                    let cell = page::parse_table_leaf_cell(bytes, &header, off)?;
                    return Ok(if cell.rowid == rowid { TableSeek::Found } else { TableSeek::Absent });
                }
                return Ok(TableSeek::End);
            }
        }
    }

    /// Current rowid (table cursor only).
    pub fn rowid(&self) -> Result<i64, ReaderError> {
        let top = *self.top()?;
        let (bytes, header) = self.page(top.pgno)?;
        let off = page::cell_offset(bytes, &header, top.cell)?;
        let cell = page::parse_table_leaf_cell(bytes, &header, off)?;
        Ok(cell.rowid)
    }

    /// Current payload (leaf entry; length is the inline payload length).
    pub fn payload(&self) -> Result<&'a [u8], ReaderError> {
        let top = *self.top()?;
        let (bytes, header) = self.page(top.pgno)?;
        let off = page::cell_offset(bytes, &header, top.cell)?;
        if self.index {
            let (_, payload) = page::parse_index_cell(bytes, off, false)?;
            Ok(payload)
        } else {
            let cell = page::parse_table_leaf_cell(bytes, &header, off)?;
            Ok(cell.payload)
        }
    }

    /// Index lower bound on the first record field (integer key): positions
    /// at the first entry whose field0 >= `key`. Returns End at past-the-end.
    pub fn index_lower_bound(&mut self, key: i64) -> Result<Step, ReaderError> {
        self.stack.clear();
        let mut pgno = self.root;
        loop {
            self.push_level(pgno)?;
            let top = *self.top()?;
            let (bytes, header) = self.page(top.pgno)?;
            if header.kind.is_interior() {
                let mut child = header.right_child.ok_or(ReaderError::Corrupt("no right child"))?;
                let mut chosen = header.ncells; // default: right pointer
                for i in 0..header.ncells {
                    let off = page::cell_offset(bytes, &header, i)?;
                    let (_, payload) = page::parse_index_cell(bytes, off, true)?;
                    let k = index_first_int(payload)?;
                    if k >= key {
                        child = page::parse_index_cell(bytes, off, true)?.0;
                        chosen = i;
                        break;
                    }
                }
                self.top_mut()?.cell = chosen;
                pgno = child;
            } else {
                for i in 0..header.ncells {
                    let off = page::cell_offset(bytes, &header, i)?;
                    let (_, payload) = page::parse_index_cell(bytes, off, false)?;
                    let k = index_first_int(payload)?;
                    if k >= key {
                        self.top_mut()?.cell = i;
                        return Ok(Step::Moved);
                    }
                }
                return Ok(Step::End);
            }
        }
    }

    /// Current index entry's first record field as an integer.
    pub fn index_key(&self) -> Result<i64, ReaderError> {
        let payload = self.payload()?;
        index_first_int(payload)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TableSeek {
    Found,
    Absent,
    End,
}

fn is_interior_kind(kind: PageKind) -> bool {
    kind.is_interior()
}

/// Decode the first field of an index record payload as a signed integer.
fn index_first_int(payload: &[u8]) -> Result<i64, ReaderError> {
    let mut fields = [crate::record::Field { serial_type: 0, offset: 0, len: 0 }; 4];
    let (_, n) = crate::record::decode_header(payload, &mut fields[..])
        .map_err(|_| ReaderError::Corrupt("index record header"))?;
    if n == 0 {
        return Err(ReaderError::Corrupt("index record without fields"));
    }
    let f = fields[0];
    let b = &payload[f.offset..f.offset + f.len];
    match f.serial_type {
        1 => Ok(b[0] as i8 as i64),
        2 => Ok(i16::from_be_bytes([b[0], b[1]]) as i64),
        3 => Ok(i24_to_i64(b)),
        4 => Ok(i32::from_be_bytes([b[0], b[1], b[2], b[3]]) as i64),
        5 => Ok(i48_to_i64(b)),
        6 => Ok(i64::from_be_bytes(b.try_into().map_err(|_| ReaderError::Corrupt("int6"))?)),
        8 => Ok(0),
        9 => Ok(1),
        0 => Err(ReaderError::Corrupt("NULL index key")),
        _ => Err(ReaderError::Corrupt("non-integer index key")),
    }
}

fn i24_to_i64(b: &[u8]) -> i64 {
    let mut v = 0i64;
    v |= (b[0] as i64) << 16;
    v |= (b[1] as i64) << 8;
    v |= b[2] as i64;
    if b[0] & 0x80 != 0 {
        v |= !0i64 << 24;
    }
    v
}

fn i48_to_i64(b: &[u8]) -> i64 {
    let mut v = 0i64;
    for (i, byte) in b.iter().take(6).enumerate() {
        v |= (*byte as i64) << (8 * (5 - i));
    }
    if b[0] & 0x80 != 0 {
        v |= !0i64 << 48;
    }
    v
}
