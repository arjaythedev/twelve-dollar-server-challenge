//! Bounds-checked SQLite record/varint decoding and a pure-Rust read-only
//! B-tree/WAL reader.
//!
//! Layer: L1 substrate. Decoders are `no_std`, zero-dep, allocation-free.
//! The reader (feature `reader`, default on) maps the database file and WAL
//! via mmap and walks B-trees without the SQL engine; correctness contract:
//! results identical to SQLite on the same snapshot (see `tests/query_oracle.rs`).
//!
//! Proof targets (Lean, `proof/`): totality, bounds, and exact output-size
//! prediction for the decoders; cursor-height bound for the walker.
//! References: https://www.sqlite.org/fileformat2.html and the amalgamation
//! (3.54.1) as the de-facto spec.

#![cfg_attr(not(feature = "reader"), no_std)]
#![forbid(unsafe_op_in_unsafe_fn)]

pub mod record;
#[cfg(feature = "reader")]
pub mod btree;
#[cfg(feature = "reader")]
pub mod mmap;
#[cfg(feature = "reader")]
pub mod page;
pub mod scan;
#[cfg(feature = "reader")]
pub mod store;
#[cfg(feature = "reader")]
pub mod wal;
pub mod varint;

/// Decode error: every failure is a bounded, total, allocation-free
/// `Result` — never a panic. A truncated or malformed buffer is data,
/// not a bug.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DecodeError {
    /// Buffer ended before the item completed.
    Truncated,
    /// A varint exceeded its 9-byte maximum.
    VarintTooLong,
    /// A serial type is not valid for a record body.
    BadSerialType,
    /// Declared record size does not match the payload available.
    SizeMismatch,
    /// Record declares more fields than the caller's output buffer holds.
    TooManyFields,
}

/// Reader error: structural problems with the file, or features this reader
/// deliberately does not support (the server falls back to bundled SQLite).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReaderError {
    /// Feature present in the file that the focused reader does not support
    /// (overflow cells, non-UTF8 encoding, reserved bytes, ...).
    Unsupported(&'static str),
    /// Page or cell structure violates the format's invariants.
    Corrupt(&'static str),
    /// Page number or offset out of range for the mapped file.
    OutOfRange,
    /// The WAL file is missing while snapshots require frames.
    WalMissing,
    /// WAL header invalid or magic unknown.
    WalHeaderBad,
    /// A frame failed the salt or checksum chain check.
    WalFrameBad,
}

impl std::fmt::Display for ReaderError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ReaderError::Unsupported(m) => write!(f, "unsupported: {m}"),
            ReaderError::Corrupt(m) => write!(f, "corrupt: {m}"),
            ReaderError::OutOfRange => write!(f, "out of range"),
            ReaderError::WalMissing => write!(f, "wal missing"),
            ReaderError::WalHeaderBad => write!(f, "wal header bad"),
            ReaderError::WalFrameBad => write!(f, "wal frame bad"),
        }
    }
}
