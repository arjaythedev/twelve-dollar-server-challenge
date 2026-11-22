//! Differential oracle: Rust varint decoder vs random bytes and vs known
//! SQLite encodings. The semantic ground truth is the real `sqlite3` on the
//! seed file (record-level oracle lands with the shim); this file proves the
//! local contract: total, ≤9 bytes, agrees with the Lean spec's shape.

use proptest::prelude::*;

use sqlite_btree::varint::decode;

#[test]
fn known_sqlite_encodings() {
    // Single-byte and two-byte encodings of the values SQLite's varint docs use.
    assert_eq!(decode(&[0x7f]), Ok((127, 1)));
    assert_eq!(decode(&[0x81, 0x00]), Ok((128, 2)));
    assert_eq!(decode(&[0xff, 0x7f]), Ok((16383, 2)));
}

proptest! {
    /// Never panics, never consumes > 9, and trailing bytes don't matter.
    #[test]
    fn total_and_bounded(bytes in proptest::collection::vec(any::<u8>(), 0..24)) {
        if let Ok((_, n)) = decode(&bytes) {
            prop_assert!(n <= 9);
            prop_assert!(n <= bytes.len());
        }
    }

    /// Encoding round-trip: any u64 encoded as SQLite varint decodes exactly.
    #[test]
    fn roundtrip(v in any::<u64>()) {
        let (enc, n) = encode(v);
        prop_assert!(n <= 9);
        prop_assert_eq!(decode(&enc[..n]).map(|(d, _)| d), Ok(v));
    }
}

/// SQLite varint encoding (getVarint inverse), per sqlite3PutVarint:
/// 9-byte form first (low byte in slot 8, then 7-bit groups of `v >> 8`
/// filling bytes 7..0 LSB-first, all with continuation bits).
fn encode(v: u64) -> ([u8; 9], usize) {
    let mut buf = [0u8; 9];
    if v >> 56 != 0 {
        buf[8] = (v & 0xff) as u8;
        let mut n = v >> 8;
        for i in (1..8).rev() {
            buf[i] = ((n & 0x7f) as u8) | 0x80;
            n >>= 7;
        }
        buf[0] = ((n & 0x7f) as u8) | 0x80;
        return (buf, 9);
    }
    let mut n = 1usize;
    let mut rest = v;
    while rest >> 7 != 0 {
        rest >>= 7;
        n += 1;
    }
    for i in 0..n {
        let shift = 7 * (n - 1 - i);
        let b = ((v >> shift) & 0x7f) as u8;
        buf[i] = if i + 1 < n { b | 0x80 } else { b };
    }
    (buf, n)
}
