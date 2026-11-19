//! Checked owned response construction, with a sticky failure bit.
//!
//! Callers must check `failed()` before publishing *any* bytes, including after
//! a serializer truncates a provisional suffix to attempt fallback. `clear()`
//! explicitly starts a new construction. The quota charges owned Vec capacity,
//! not just its logical length; scratch and final output need separate quotas
//! from the caller's aggregate reservation. This does not account allocator
//! bookkeeping or temporary memory inside realloc.
//!
//! `try_reserve_exact` can return allocator-supplied excess capacity (Rust Vec
//! documentation). Unexpected capacity above quota is immediately discarded
//! and fails construction; it is never retained/published. The quota is not a
//! promise that an allocator cannot transiently overallocate during a call.

use std::ops::Deref;

/// Minimal serializer interface. Vec compatibility is deliberately unchecked;
/// bounded production construction must use Buffer and inspect its failure bit.
pub trait Output {
    fn push(&mut self, byte: u8);
    fn extend_from_slice(&mut self, bytes: &[u8]);
    fn reserve(&mut self, additional: usize);
    fn len(&self) -> usize;
    fn truncate(&mut self, len: usize);
    fn write_at(&mut self, offset: usize, bytes: &[u8]);
    fn failed(&self) -> bool { false }
}

impl Output for Vec<u8> {
    #[inline]
    fn push(&mut self, byte: u8) { Vec::push(self, byte); }
    #[inline]
    fn extend_from_slice(&mut self, bytes: &[u8]) { Vec::extend_from_slice(self, bytes); }
    #[inline]
    fn reserve(&mut self, additional: usize) { Vec::reserve(self, additional); }
    #[inline]
    fn len(&self) -> usize { Vec::len(self) }
    #[inline]
    fn truncate(&mut self, len: usize) { Vec::truncate(self, len); }
    #[inline]
    fn write_at(&mut self, offset: usize, bytes: &[u8]) {
        self[offset..offset + bytes.len()].copy_from_slice(bytes);
    }
}

#[derive(Debug)]
pub struct Buffer {
    bytes: Vec<u8>,
    limit: usize,
    failed: bool,
}

impl Buffer {
    pub fn new(limit: usize) -> Self {
        Self { bytes: Vec::new(), limit, failed: false }
    }

    /// Takes ownership without copying. An over-quota retained allocation is
    /// discarded and marked failed, even if its logical length would fit.
    pub fn from_vec(bytes: Vec<u8>, limit: usize) -> Self {
        if bytes.capacity() > limit {
            Self { bytes: Vec::new(), limit, failed: true }
        } else {
            Self { bytes, limit, failed: false }
        }
    }

    /// Changes the quota without resetting an existing failure. If retained
    /// capacity exceeds the new quota, discards the complete old construction
    /// and marks failure. Call clear() explicitly before a new response. This
    /// prevents an idle reused buffer from evading aggregate capacity accounting.
    pub fn set_limit(&mut self, limit: usize) {
        self.limit = limit;
        if self.bytes.capacity() > limit {
            self.bytes = Vec::new();
            self.failed = true;
        }
    }

    #[inline]
    pub fn failed(&self) -> bool { self.failed }
    #[inline]
    pub fn capacity(&self) -> usize { self.bytes.capacity() }
    #[inline]
    pub fn len(&self) -> usize { self.bytes.len() }
    #[inline]
    pub fn as_slice(&self) -> &[u8] { self.bytes.as_slice() }

    /// Starts a fresh construction, preserving only an in-quota allocation.
    pub fn clear(&mut self) {
        self.bytes.clear();
        self.failed = false;
    }

    /// Transfers ownership, not permission to publish. Check failed() first;
    /// the vector may contain an incomplete response when construction failed.
    pub fn into_vec(self) -> Vec<u8> { self.bytes }

    /// Ensures len + additional bytes fit, without panicking on capacity errors.
    /// Growth is geometric when possible but always requested within the quota.
    #[inline(always)]
    pub fn reserve(&mut self, additional: usize) {
        if !self.failed && additional <= self.bytes.capacity() - self.bytes.len() {
            return;
        }
        self.reserve_slow(additional);
    }

    #[cold]
    #[inline(never)]
    fn reserve_slow(&mut self, additional: usize) {
        if self.failed { return; }
        let required = match self.bytes.len().checked_add(additional) {
            Some(required) if required <= self.limit => required,
            _ => { self.failed = true; return; }
        };
        if required <= self.bytes.capacity() { return; }
        let doubled = self.bytes.capacity().saturating_mul(2).max(64);
        let target = required.max(doubled.min(self.limit));
        // target >= required > capacity >= len, so this subtraction cannot wrap.
        if self.bytes.try_reserve_exact(target - self.bytes.len()).is_err() {
            self.failed = true;
        } else if self.bytes.capacity() > self.limit {
            // Vec does not contractually guarantee an exact allocator capacity.
            self.bytes = Vec::new();
            self.failed = true;
        }
    }

    #[inline(always)]
    pub fn push(&mut self, byte: u8) {
        if !self.failed && self.bytes.len() < self.bytes.capacity() {
            self.bytes.push(byte);
        } else {
            self.reserve_slow(1);
            if !self.failed { self.bytes.push(byte); }
        }
    }

    #[inline(always)]
    pub fn extend_from_slice(&mut self, bytes: &[u8]) {
        if !self.failed && bytes.len() <= self.bytes.capacity() - self.bytes.len() {
            self.bytes.extend_from_slice(bytes);
        } else {
            self.reserve_slow(bytes.len());
            if !self.failed { self.bytes.extend_from_slice(bytes); }
        }
    }

    /// Removes provisional bytes; does not repair allocation/quota failure.
    #[inline(always)]
    pub fn truncate(&mut self, len: usize) { self.bytes.truncate(len); }

    #[inline(always)]
    pub fn write_at(&mut self, offset: usize, bytes: &[u8]) {
        if !self.failed {
            self.bytes[offset..offset + bytes.len()].copy_from_slice(bytes);
        }
    }
}

impl Output for Buffer {
    #[inline(always)]
    fn failed(&self) -> bool { Buffer::failed(self) }
    #[inline(always)]
    fn push(&mut self, byte: u8) { Buffer::push(self, byte); }
    #[inline(always)]
    fn extend_from_slice(&mut self, bytes: &[u8]) { Buffer::extend_from_slice(self, bytes); }
    #[inline(always)]
    fn reserve(&mut self, additional: usize) { Buffer::reserve(self, additional); }
    #[inline(always)]
    fn len(&self) -> usize { Buffer::len(self) }
    #[inline(always)]
    fn truncate(&mut self, len: usize) { Buffer::truncate(self, len); }
    #[inline(always)]
    fn write_at(&mut self, offset: usize, bytes: &[u8]) { Buffer::write_at(self, offset, bytes); }
}

impl Deref for Buffer {
    type Target = [u8];
    fn deref(&self) -> &[u8] { self.as_slice() }
}

#[cfg(test)]
mod tests {
    use super::{Buffer, Output};

    #[test]
    fn quota_failure_is_atomic_and_sticky() {
        let mut out = Buffer::new(4);
        out.extend_from_slice(b"abc");
        out.extend_from_slice(b"de");
        assert!(out.failed());
        assert_eq!(out.as_slice(), b"abc");
        out.push(b'd');
        out.reserve(0);
        assert_eq!(out.as_slice(), b"abc");
        assert!(out.capacity() <= 4);
        out.clear();
        out.extend_from_slice(b"abcd");
        assert!(!out.failed());
        assert_eq!(out.as_slice(), b"abcd");
    }

    #[test]
    fn arithmetic_overflow_does_not_allocate() {
        let mut out = Buffer::new(8);
        out.push(b'x');
        let cap = out.capacity();
        out.reserve(usize::MAX);
        assert!(out.failed());
        assert_eq!(out.as_slice(), b"x");
        assert_eq!(out.capacity(), cap);
    }

    #[test]
    fn impossible_vec_capacity_fails_without_large_allocation() {
        // Vec's isize::MAX size restriction rejects this before allocator work.
        let mut out = Buffer::new(usize::MAX);
        out.reserve(usize::MAX);
        assert!(out.failed());
        assert_eq!(out.capacity(), 0);
    }

    #[test]
    fn truncate_and_raised_limit_cannot_hide_failure() {
        let mut out = Buffer::new(3);
        out.extend_from_slice(b"abc");
        out.push(b'd');
        out.truncate(0);
        out.set_limit(100);
        out.extend_from_slice(b"fallback");
        assert!(out.failed());
        assert!(out.is_empty());
    }

    #[test]
    fn lowering_limit_discards_over_quota_retention() {
        let mut vec = Vec::with_capacity(128);
        vec.push(b'x');
        let mut out = Buffer::from_vec(vec, 128);
        assert!(!out.failed());
        out.set_limit(16);
        assert!(out.failed());
        assert_eq!(out.capacity(), 0);
        assert!(out.is_empty());
        out.clear();
        out.extend_from_slice(b"fresh");
        assert!(!out.failed());
        assert!(out.capacity() <= 16);
    }

    #[test]
    fn from_vec_checks_capacity_not_length() {
        let vec = Vec::with_capacity(128);
        let out = Buffer::from_vec(vec, 16);
        assert!(out.failed());
        assert_eq!(out.capacity(), 0);
    }

    #[test]
    fn fitting_ownership_transfer_preserves_allocation() {
        let vec = b"hello".to_vec();
        let pointer = vec.as_ptr();
        let out = Buffer::from_vec(vec, 64);
        assert!(!out.failed());
        assert_eq!(out.as_ptr(), pointer);
        assert_eq!(out.into_vec(), b"hello");
    }

    #[test]
    fn generic_serializer_matches_vec() {
        fn serialize(out: &mut impl Output) {
            out.reserve(10);
            out.extend_from_slice(b"discard");
            out.truncate(0);
            out.push(b'[');
            out.extend_from_slice(b"123");
            assert_eq!(out.len(), 4);
            out.push(b']');
        }
        let mut vec = Vec::new();
        let mut bounded = Buffer::new(32);
        serialize(&mut vec);
        serialize(&mut bounded);
        assert!(!bounded.failed());
        assert_eq!(bounded.as_slice(), vec.as_slice());
    }

    #[test]
    fn geometric_growth_never_retains_beyond_limit() {
        let mut out = Buffer::new(257);
        for _ in 0..257 {
            out.push(b'x');
            assert!(!out.failed());
            assert!(out.capacity() <= 257);
        }
        out.push(b'x');
        assert!(out.failed());
        assert_eq!(out.len(), 257);
    }
}
