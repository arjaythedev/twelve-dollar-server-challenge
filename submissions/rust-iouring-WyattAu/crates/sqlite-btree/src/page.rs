//! SQLite page parsing (fileformat2.html section 1.3): page headers, cell
//! pointer arrays, and cell layouts for the four page kinds. Bounds-checked:
//! every offset read is validated against the page bytes.

use crate::ReaderError;

pub const PAGE_INTERIOR_INDEX: u8 = 0x02;
pub const PAGE_INTERIOR_TABLE: u8 = 0x05;
pub const PAGE_LEAF_INDEX: u8 = 0x0a;
pub const PAGE_LEAF_TABLE: u8 = 0x0d;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PageKind {
    InteriorIndex,
    InteriorTable,
    LeafIndex,
    LeafTable,
}

impl PageKind {
    pub fn is_interior(self) -> bool {
        matches!(self, PageKind::InteriorIndex | PageKind::InteriorTable)
    }
}

#[derive(Debug, Clone, Copy)]
pub struct PageHeader {
    pub kind: PageKind,
    pub ncells: usize,
    /// Offset of the cell pointer array (after the page header, plus 100 on page 1).
    pub ptr_array: usize,
    /// Right-most child pointer (interior pages only).
    pub right_child: Option<u32>,
}

/// Cell pointer at `idx`: absolute offset within the page bytes.
#[inline]
pub fn cell_offset(page: &[u8], header: &PageHeader, idx: usize) -> Result<usize, ReaderError> {
    if idx >= header.ncells {
        return Err(ReaderError::Corrupt("cell index past count"));
    }
    let p = header.ptr_array + idx * 2;
    let off = (be16(page, p).ok_or(ReaderError::OutOfRange)? as usize) & 0xffff;
    if off < header.ptr_array || off >= page.len() {
        return Err(ReaderError::Corrupt("cell offset out of page"));
    }
    Ok(off)
}

/// Parse a page header. `page` is the full page bytes; `pgno` is its number
/// (page 1 carries the 100-byte database header before the page header).
pub fn parse_page(page: &[u8], pgno: u32) -> Result<PageHeader, ReaderError> {
    let h = if pgno == 1 { 100 } else { 0 };
    if page.len() < h + 8 {
        return Err(ReaderError::OutOfRange);
    }
    let kind = page[h];
    let interior = match kind {
        PAGE_INTERIOR_INDEX | PAGE_INTERIOR_TABLE => true,
        PAGE_LEAF_INDEX | PAGE_LEAF_TABLE => false,
        other => return Err(ReaderError::Unsupported(unknown_page(other))),
    };
    let ncells = be16(page, h + 3).ok_or(ReaderError::OutOfRange)? as usize;
    let header_len = if interior { 12 } else { 8 };
    if page.len() < h + header_len {
        return Err(ReaderError::OutOfRange);
    }
    let right_child = if interior {
        Some(be32(page, h + 8).ok_or(ReaderError::OutOfRange)?)
    } else {
        None
    };
    if interior && ncells == 0 && right_child == Some(0) {
        return Err(ReaderError::Corrupt("interior page without children"));
    }
    Ok(PageHeader { kind: if interior { kind_of(interior, index_kind(kind)) } else { kind_of(false, index_kind(kind)) }, ncells, ptr_array: h + header_len, right_child })
}

fn unknown_page(k: u8) -> &'static str {
    match k {
        0x01 | 0x03 => "freelist leaf/trunk page in btree position",
        0x04 => "unknown page kind 0x04",
        _ => "unknown page kind",
    }
}

fn index_kind(kind: u8) -> bool {
    kind == PAGE_INTERIOR_INDEX || kind == PAGE_LEAF_INDEX
}

fn kind_of(interior: bool, index: bool) -> PageKind {
    match (interior, index) {
        (true, true) => PageKind::InteriorIndex,
        (true, false) => PageKind::InteriorTable,
        (false, true) => PageKind::LeafIndex,
        (false, false) => PageKind::LeafTable,
    }
}

/// Table leaf cell: payload-length varint, rowid varint, inline payload.
/// Overflow is `Unsupported` (server falls back; bodies <= 500 B never spill).
pub struct TableCell<'a> {
    pub rowid: i64,
    pub payload: &'a [u8],
}

pub fn parse_table_leaf_cell<'a>(
    page: &'a [u8],
    header: &PageHeader,
    off: usize,
) -> Result<TableCell<'a>, ReaderError> {
    let (payload_len, n) = crate::varint::decode(&page[off..]).map_err(map_varint)?;
    let (rowid, n2) = crate::varint::decode(&page[off + n..]).map_err(map_varint)?;
    let payload_start = off + n + n2;
    let usable = page.len();
    // Overflow threshold math (fileformat2.html): maxLocal = usable - 35.
    let max_local = usable - 35;
    if payload_len as usize > max_local {
        return Err(ReaderError::Unsupported("overflow cell"));
    }
    let end = payload_start + payload_len as usize;
    if end > page.len() {
        return Err(ReaderError::Corrupt("table cell payload past page"));
    }
    Ok(TableCell { rowid: rowid as i64, payload: &page[payload_start..end] })
}

/// Table interior cell: left-child page number + rowid key.
pub fn parse_table_interior_cell(page: &[u8], off: usize) -> Result<(u32, i64), ReaderError> {
    if off + 4 > page.len() {
        return Err(ReaderError::Corrupt("interior cell truncated"));
    }
    let child = be32(page, off).ok_or(ReaderError::OutOfRange)?;
    let (key, _) = crate::varint::decode(&page[off + 4..]).map_err(map_varint)?;
    Ok((child, key as i64))
}

/// Index cell (leaf or interior): payload-length varint + payload; interior
/// cells additionally carry the left-child pointer first.
pub fn parse_index_cell<'a>(
    page: &'a [u8],
    off: usize,
    interior: bool,
) -> Result<(u32, &'a [u8]), ReaderError> {
    let mut p = off;
    let child = if interior {
        let c = be32(page, p).ok_or(ReaderError::OutOfRange)?;
        p += 4;
        c
    } else {
        0
    };
    let (payload_len, n) = crate::varint::decode(&page[p..]).map_err(map_varint)?;
    let start = p + n;
    // Overflow threshold for index pages: maxLocal = ((usable-12)*64/255)-23.
    let usable = page.len();
    let max_local = ((usable - 12) * 64 / 255) - 23;
    if payload_len as usize > max_local {
        return Err(ReaderError::Unsupported("overflow index cell"));
    }
    let end = start + payload_len as usize;
    if end > page.len() {
        return Err(ReaderError::Corrupt("index cell payload past page"));
    }
    Ok((child, &page[start..end]))
}

#[inline]
pub fn be16(page: &[u8], off: usize) -> Option<u16> {
    let b = page.get(off..off + 2)?;
    Some(u16::from_be_bytes([b[0], b[1]]))
}

#[inline]
pub fn be32(page: &[u8], off: usize) -> Option<u32> {
    let b = page.get(off..off + 4)?;
    Some(u32::from_be_bytes([b[0], b[1], b[2], b[3]]))
}

fn map_varint(e: crate::DecodeError) -> ReaderError {
    match e {
        crate::DecodeError::Truncated => ReaderError::Corrupt("varint past page end"),
        other => ReaderError::Corrupt( varint_msg(other)),
    }
}

fn varint_msg(e: crate::DecodeError) -> &'static str {
    match e {
        crate::DecodeError::VarintTooLong => "varint too long",
        _ => "varint malformed",
    }
}
