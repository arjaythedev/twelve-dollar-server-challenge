use std::{
    io, slice,
    sync::atomic::{AtomicU16, Ordering},
};

use crate::{mmap::Mmap, sys::io_uring};

const MAX_ENTRIES: u16 = 32768;
pub struct BufferId(u16);

impl BufferId {
    pub(crate) fn from_cqe_flags(flags: u32) -> Option<Self> {
        if flags & io_uring::IORING_CQE_F_BUFFER == 0 {
            return None;
        }
        Some(Self((flags >> io_uring::IORING_CQE_BUFFER_SHIFT) as u16))
    }
}

pub struct BufRing {
    // array of descriptors (entries)
    ring: Mmap,
    // where the actual bytes live
    pool: Mmap,
    entries: u16,
    buf_size: u32,
    mask: u16,
    local_tail: u16,
}

impl BufRing {
    // buffer ring id
    pub const BGID: u16 = 0;

    pub fn new(entries: u16, buf_size: u32) -> io::Result<Self> {
        let entries = entries.min(MAX_ENTRIES).next_power_of_two();
        let ring_len = entries as usize * size_of::<io_uring::io_uring_buf>();
        let ring = Mmap::anon(ring_len)?;
        let pool_len = buf_size as usize * entries as usize;
        let pool = Mmap::anon(pool_len)?;

        let mut bf = Self {
            ring,
            pool,
            entries,
            mask: entries - 1,
            buf_size,
            local_tail: 0,
        };
        for bid in 0..entries {
            bf.push(bid);
        }
        bf.publish();
        Ok(bf)
    }

    pub fn ring_addr(&self) -> u64 {
        self.ring.as_ptr() as u64
    }

    pub fn entries(&self) -> u16 {
        self.entries
    }

    pub fn get(&self, id: &BufferId, len: usize) -> &[u8] {
        debug_assert!(id.0 < self.entries);
        debug_assert!(len <= self.buf_size as usize);
        let offset = id.0 as usize * self.buf_size as usize;
        unsafe { slice::from_raw_parts(self.pool.as_ptr().add(offset), len) }
    }

    fn push(&mut self, bid: u16) {
        let idx = (self.local_tail & self.mask) as usize;
        unsafe {
            let slot = self.ring.as_ptr().cast::<io_uring::io_uring_buf>().add(idx);

            let offset = bid as usize * self.buf_size as usize;
            let pool_addr = self.pool.as_ptr().add(offset);

            (*slot).addr = pool_addr as u64;
            (*slot).len = self.buf_size;
            (*slot).bid = bid;
        }
        self.local_tail = self.local_tail.wrapping_add(1);
    }

    pub fn recycle(&mut self, id: BufferId) {
        self.push(id.0);
        self.publish();
    }

    fn publish(&self) {
        // The ring has no separate header, the tail is stored in bufs[0].resv (byte 14 of the ring).
        unsafe {
            let atomic = &*self.ring.as_ptr().add(14).cast::<AtomicU16>();
            atomic.store(self.local_tail, Ordering::Release);
        }
    }
}
