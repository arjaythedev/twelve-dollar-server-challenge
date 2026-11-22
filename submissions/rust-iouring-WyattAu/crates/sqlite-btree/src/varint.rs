//! SQLite varint (HLA: "header length is a variable-length integer").
//!
//! Encoding: 1–9 bytes, big-endian, 7 bits per byte with high bit as
//! continuation flag; the 9th byte contributes all 8 bits.
//!
//! Lean target: `decode` is total, returns at most 9 bytes consumed, and
//! agrees with SQLite's `getVarint` on every input.

use crate::DecodeError;

/// Decode a SQLite varint from the front of `buf`.
/// Returns `(value, bytes_consumed)`.
///
/// Maximum input: 9 bytes. Consumes ≤ 9 bytes, never panics.
#[inline]
pub fn decode(buf: &[u8]) -> Result<(u64, usize), DecodeError> {
    let mut value: u64 = 0;
    for (i, &byte) in buf.iter().take(9).enumerate() {
        if i == 8 {
            // 9th byte: all 8 bits are payload.
            return Ok(((value << 8) | byte as u64, 9));
        }
        value = (value << 7) | (byte & 0x7f) as u64;
        if byte & 0x80 == 0 {
            return Ok((value, i + 1));
        }
    }
    // Ran off the end (or empty buffer) with continuation bits still set.
    Err(if buf.len() < 9 {
        DecodeError::Truncated
    } else {
        DecodeError::VarintTooLong
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn single_byte() {
        assert_eq!(decode(&[0x00]), Ok((0, 1)));
        assert_eq!(decode(&[0x7f]), Ok((127, 1)));
    }

    #[test]
    fn two_bytes() {
        // 0x81 0x00 -> (1 << 7) | 0 = 128
        assert_eq!(decode(&[0x81, 0x00]), Ok((128, 2)));
        // 0xff 0x7f -> (127 << 7) | 127 = 16383
        assert_eq!(decode(&[0xff, 0x7f]), Ok((16383, 2)));
    }

    #[test]
    fn nine_byte_cap() {
        // SQLite's u64::MAX encoding is 9 bytes: ff x8 then ff.
        let max9 = [0xff_u8; 9];
        assert_eq!(decode(&max9), Ok((u64::MAX, 9)));
    }

    #[test]
    fn truncated_is_data_not_panic() {
        assert_eq!(decode(&[]), Err(DecodeError::Truncated));
        assert_eq!(decode(&[0xff]), Err(DecodeError::Truncated));
        assert_eq!(decode(&[0xff_u8; 8]), Err(DecodeError::Truncated));
    }

    #[test]
    fn never_consumes_more_than_nine() {
        // 10 continuation bytes: still stops at 9.
        let buf = [0xff_u8; 16];
        let (_, n) = decode(&buf).unwrap();
        assert_eq!(n, 9);
    }
}
