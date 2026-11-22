//! SQLite record body decoding: serial types → field views, zero-copy.
//!
//! Record layout: `header_size (varint)` then header serial types, then
//! body. Serial types (fileformat2.html §2.1):
//!
//! | type | meaning | body size |
//! |---|---|---|
//! | 0 | NULL | 0 |
//! | 1..=6 | int, 1/2/3/4/6/8 bytes | as listed |
//! | 7 | IEEE f64, big-endian | 8 |
//! | 8 | integer 0 | 0 |
//! | 9 | integer 1 | 0 |
//! | 10, 11 | reserved | — reject |
//! | ≥ 12, even | BLOB, (n−12)/2 bytes | (n−12)/2 |
//! | ≥ 13, odd | TEXT, (n−13)/2 bytes | (n−13)/2 |
//!
//! Lean targets: `field_offset` is total; sum of body sizes computed from
//! the header equals the declared payload size (no OOB); and for our fixed
//! schema (`users`, `posts`, `likes`, two indexes) the record-shape checks
//! at startup imply every later decode yields exactly the expected field
//! count and types.

use crate::{varint, DecodeError};

/// A decoded field: byte range into the record body plus its serial type.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Field {
    pub serial_type: u64,
    /// Offset and length within the record body (not the whole page).
    pub offset: usize,
    pub len: usize,
}

/// Body byte-length contributed by a serial type. Total, pure, branch-light.
#[inline]
pub const fn serial_type_body_len(serial_type: u64) -> Option<usize> {
    match serial_type {
        0 | 8 | 9 => Some(0),
        1 => Some(1),
        2 => Some(2),
        3 => Some(3),
        4 => Some(4),
        5 => Some(6),
        6 => Some(8),
        7 => Some(8),
        10 | 11 => None, // reserved, reject
        n => {
            // (n - 12) / 2; on 64-bit targets usize == u64 so no truncation.
            // Garbage-sized types are caught by the body_off bounds check.
            Some(((n.wrapping_sub(12)) / 2) as usize)
        }
    }
}

/// Decode the header of a record body into `out`, allocation-free.
/// Returns `(body_len, field_count)` where `body_len` is the total record
/// body length the header declares and `field_count` is how many entries of
/// `out` were populated. `buf` is the full record payload (header + body).
/// Returns `DecodeError::SizeMismatch`-adjacent `TooManyFields` when `out`
/// is shorter than the header declares.
pub fn decode_header(buf: &[u8], out: &mut [Field]) -> Result<(usize, usize), DecodeError> {
    let (header_size, mut pos) = varint::decode(buf).map_err(|_| DecodeError::Truncated)?;
    if header_size < (pos as u64) || header_size as usize > buf.len() {
        return Err(DecodeError::SizeMismatch);
    }
    let header_end = header_size as usize;
    let mut count = 0usize;
    let mut body_off = header_end;
    while pos < header_end {
        let (st, n) = varint::decode(&buf[pos..]).map_err(|_| DecodeError::Truncated)?;
        pos += n;
        let len = serial_type_body_len(st).ok_or(DecodeError::BadSerialType)?;
        if count >= out.len() {
            return Err(DecodeError::TooManyFields);
        }
        out[count] = Field { serial_type: st, offset: body_off, len };
        count += 1;
        body_off += len;
    }
    if pos != header_end {
        return Err(DecodeError::SizeMismatch);
    }
    if body_off > buf.len() {
        return Err(DecodeError::SizeMismatch);
    }
    Ok((body_off, count))
}

// TODO(challenge): record → JSON direct encoder for the `posts` shape
// (id, body, created_at, author-name via users lookup, like_count), with an
// exact output-size prepass so the server can send from one buffer.
// TODO(challenge): differential oracle tests against the seed file.
