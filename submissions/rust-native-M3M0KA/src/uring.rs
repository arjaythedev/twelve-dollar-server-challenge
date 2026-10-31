//! Raw io_uring bindings: ring setup, submission, completion reaping, fixed
//! file tables, provided buffer rings and registration.
//!
//! Written against the kernel UAPI directly (no liburing, no crates). The
//! structures below mirror `linux/io_uring.h`; every field offset is asserted
//! at startup by the self-test, so an ABI mismatch disables the backend rather
//! than corrupting memory.

use std::os::raw::{c_int, c_void};

pub const SYS_SETUP: i64 = 425;
pub const SYS_ENTER: i64 = 426;
pub const SYS_REGISTER: i64 = 427;

pub const IORING_OFF_SQ_RING: i64 = 0;
pub const IORING_OFF_CQ_RING: i64 = 0x0800_0000;
pub const IORING_OFF_SQES: i64 = 0x1000_0000;

pub const IORING_SETUP_SINGLE_ISSUER: u32 = 1 << 12;
pub const IORING_SETUP_DEFER_TASKRUN: u32 = 1 << 13;
pub const IORING_SETUP_COOP_TASKRUN: u32 = 1 << 8;
pub const IORING_SETUP_TASKRUN_FLAG: u32 = 1 << 14;
pub const IORING_SETUP_CQSIZE: u32 = 1 << 3;

pub const IORING_ENTER_GETEVENTS: u32 = 1;
pub const IORING_ENTER_SQ_WAKEUP: u32 = 2;

pub const IORING_REGISTER_FILES: u32 = 2;
pub const IORING_UNREGISTER_FILES: u32 = 3;
pub const IORING_REGISTER_EVENTFD_ASYNC: u32 = 20;
pub const IORING_REGISTER_PBUF_RING: u32 = 22;

pub const IORING_OP_NOP: u8 = 0;
pub const IORING_OP_ACCEPT: u8 = 13;
pub const IORING_OP_CLOSE: u8 = 19;
pub const IORING_OP_SEND: u8 = 26;
pub const IORING_OP_SHUTDOWN: u8 = 34;
pub const IORING_OP_RECV_MULTISHOT: u8 = 47;
pub const IORING_OP_PROVIDE_BUFFERS: u8 = 31;

pub const IOSQE_FIXED_FILE: u8 = 1 << 0;
pub const IOSQE_IO_HARDLINK: u8 = 1 << 4;
pub const IOSQE_BUFFER_SELECT: u8 = 1 << 5;
pub const IOSQE_CQE_SKIP_SUCCESS: u8 = 1 << 6;
pub const IOSQE_IO_LINK: u8 = 1 << 2;

pub const IORING_ACCEPT_MULTISHOT: u16 = 1;
pub const IORING_FILE_INDEX_ALLOC: i32 = -1;

pub const IORING_CQE_F_BUFFER: u32 = 1;
pub const IORING_CQE_F_MORE: u32 = 2;
pub const IORING_CQE_BUFFER_SHIFT: u32 = 16;

pub const IORING_ASYNC_CANCEL_ANY: u32 = 1 << 3;

pub const MAP_SHARED: c_int = 1;
pub const MAP_POPULATE: c_int = 0x8000;
pub const PROT_READ: c_int = 1;
pub const PROT_WRITE: c_int = 2;

extern "C" {
    fn syscall(num: i64, ...) -> i64;
    fn mmap(addr: *mut c_void, len: usize, prot: c_int, flags: c_int, fd: c_int, off: i64)
        -> *mut c_void;
    fn munmap(addr: *mut c_void, len: usize) -> c_int;
    fn eventfd(initval: u32, flags: c_int) -> c_int;
}

#[repr(C)]
#[derive(Default)]
pub struct SqOffsets {
    pub head: u32,
    pub tail: u32,
    pub ring_mask: u32,
    pub ring_entries: u32,
    pub flags: u32,
    pub dropped: u32,
    pub array: u32,
    pub resv1: u32,
    pub resv2: u64,
}

#[repr(C)]
#[derive(Default)]
pub struct CqOffsets {
    pub head: u32,
    pub tail: u32,
    pub ring_mask: u32,
    pub ring_entries: u32,
    pub overflow: u32,
    pub cqes: u32,
    pub flags: u32,
    pub resv1: u32,
    pub resv2: u64,
}

#[repr(C)]
#[derive(Default)]
pub struct Params {
    pub sq_entries: u32,
    pub cq_entries: u32,
    pub flags: u32,
    pub sq_thread_cpu: u32,
    pub sq_thread_idle: u32,
    pub features: u32,
    pub wq_fd: u32,
    pub resv: [u32; 3],
    pub sq_off: SqOffsets,
    pub cq_off: CqOffsets,
}

#[repr(C)]
#[derive(Default)]
pub struct BufReg {
    pub ring_addr: u64,
    pub ring_entries: u32,
    pub bgid: u16,
    pub flags: u16,
    pub resv: [u64; 3],
}

/// A provided-buffer ring entry. Userspace owns the tail; the kernel owns head.
#[repr(C, align(8))]
#[derive(Clone, Copy, Default)]
pub struct BufEntry {
    pub addr: u64,
    pub len: u32,
    pub bid: u16,
    pub resv: u16,
}

/// SQE field offsets, from `struct io_uring_sqe`.
const O_OPCODE: usize = 0;
const O_FLAGS: usize = 1;
const O_IOPRIO: usize = 2;
const O_FD: usize = 4;
const O_OFF: usize = 8;
const O_ADDR: usize = 16;
const O_LEN: usize = 24;
const O_FLAGS32: usize = 28;
const O_USER: usize = 32;
const O_BUF_GROUP: usize = 40;
const O_FILE_INDEX: usize = 44;

const SQE_SIZE: usize = 64;
const CQE_SIZE: usize = 16;

pub struct Ring {
    pub fd: c_int,
    sq_head: *mut u32,
    sq_tail: *mut u32,
    sq_mask: u32,
    sq_array: *mut u32,
    sqes: *mut u8,
    sq_entries: u32,
    cq_head: *mut u32,
    cq_tail: *mut u32,
    cq_mask: u32,
    cqes: *mut u8,
    cq_entries: u32,
    sq_map: *mut c_void,
    sq_map_len: usize,
    cq_map: *mut c_void,
    cq_map_len: usize,
    sqes_map: *mut c_void,
    sqes_map_len: usize,
    cached_tail: u32,
}

impl Ring {
    /// Creates a ring with `entries` submission slots. Returns None when the
    /// kernel refuses io_uring (seccomp, old kernel) or the ABI looks wrong.
    pub fn new(entries: u32, cq_entries: u32, flags: u32) -> Option<Ring> {
        // The flags were previously accepted and dropped, so an older kernel's
        // refusal was indistinguishable from success. Try the requested flags,
        // then once without them, so an unsupported combination degrades to a
        // working ring instead of a silent fallback to the other transport.
        let mut fd = -1;
        for attempt in 0..2 {
            let mut p = Params { ..Default::default() };
            p.flags = if attempt == 0 { flags } else { 0 };
            p.cq_entries = cq_entries;
            fd = unsafe { syscall(SYS_SETUP, entries as i64, &mut p as *mut Params) } as c_int;
            if fd >= 0 {
                return Self::map(fd, &p);
            }
            if flags == 0 {
                break;
            }
        }
        None
    }

    fn map(fd: c_int, params: &Params) -> Option<Ring> {
        let p = Params {
            sq_entries: params.sq_entries,
            cq_entries: params.cq_entries,
            flags: params.flags,
            sq_thread_cpu: params.sq_thread_cpu,
            sq_thread_idle: params.sq_thread_idle,
            features: params.features,
            wq_fd: params.wq_fd,
            resv: params.resv,
            sq_off: SqOffsets {
                head: params.sq_off.head,
                tail: params.sq_off.tail,
                ring_mask: params.sq_off.ring_mask,
                ring_entries: params.sq_off.ring_entries,
                flags: params.sq_off.flags,
                dropped: params.sq_off.dropped,
                array: params.sq_off.array,
                resv1: params.sq_off.resv1,
                resv2: params.sq_off.resv2,
            },
            cq_off: CqOffsets {
                head: params.cq_off.head,
                tail: params.cq_off.tail,
                ring_mask: params.cq_off.ring_mask,
                ring_entries: params.cq_off.ring_entries,
                overflow: params.cq_off.overflow,
                cqes: params.cq_off.cqes,
                flags: params.cq_off.flags,
                resv1: params.cq_off.resv1,
                resv2: params.cq_off.resv2,
            },
        };
        if p.sq_entries == 0 || p.cq_entries == 0 || p.sq_off.array as usize >= 4096 {
            unsafe { super::sys::close(fd) };
            return None;
        }

        let sq_map_len = p.sq_off.array as usize + p.sq_entries as usize * 4;
        let cq_map_len = p.cq_off.cqes as usize + p.cq_entries as usize * CQE_SIZE;
        let sqes_len = p.sq_entries as usize * SQE_SIZE;

        unsafe {
            let sq = mmap(
                std::ptr::null_mut(),
                sq_map_len,
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_POPULATE,
                fd,
                IORING_OFF_SQ_RING,
            );
            if sq as isize == -1 {
                super::sys::close(fd);
                return None;
            }
            let cq = mmap(
                std::ptr::null_mut(),
                cq_map_len,
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_POPULATE,
                fd,
                IORING_OFF_CQ_RING,
            );
            if cq as isize == -1 {
                munmap(sq, sq_map_len);
                super::sys::close(fd);
                return None;
            }
            let sqes = mmap(
                std::ptr::null_mut(),
                sqes_len,
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_POPULATE,
                fd,
                IORING_OFF_SQES,
            );
            if sqes as isize == -1 {
                munmap(sq, sq_map_len);
                munmap(cq, cq_map_len);
                super::sys::close(fd);
                return None;
            }

            let base = sq as *mut u8;
            let start_tail = *(base.add(p.sq_off.tail as usize) as *const u32);
            let ring = Ring {
                fd,
                sq_head: base.add(p.sq_off.head as usize) as *mut u32,
                sq_tail: base.add(p.sq_off.tail as usize) as *mut u32,
                sq_mask: *(base.add(p.sq_off.ring_mask as usize) as *const u32),
                sq_array: base.add(p.sq_off.array as usize) as *mut u32,
                sqes: sqes as *mut u8,
                sq_entries: p.sq_entries,
                cq_head: (cq as *mut u8).add(p.cq_off.head as usize) as *mut u32,
                cq_tail: (cq as *mut u8).add(p.cq_off.tail as usize) as *mut u32,
                cq_mask: *((cq as *mut u8).add(p.cq_off.ring_mask as usize) as *const u32),
                cqes: (cq as *mut u8).add(p.cq_off.cqes as usize),
                cq_entries: p.cq_entries,
                sq_map: sq,
                sq_map_len,
                cq_map: cq,
                cq_map_len,
                sqes_map: sqes,
                sqes_map_len: sqes_len,
                cached_tail: start_tail,
            };
            Some(ring)
        }
    }

    pub fn register_files(&self, fds: &[c_int]) -> bool {
        let rc = unsafe {
            syscall(
                SYS_REGISTER,
                self.fd as i64,
                IORING_REGISTER_FILES as i64,
                fds.as_ptr() as i64,
                fds.len() as i64,
            )
        };
        rc == 0
    }

    pub fn unregister_files(&self) {
        unsafe {
            syscall(SYS_REGISTER, self.fd as i64, IORING_UNREGISTER_FILES as i64, 0, 0);
        }
    }

    pub fn register_eventfd_async(&self, efd: c_int) -> bool {
        let rc = unsafe {
            syscall(
                SYS_REGISTER,
                self.fd as i64,
                IORING_REGISTER_EVENTFD_ASYNC as i64,
                &efd as *const c_int as i64,
                1,
            )
        };
        rc == 0
    }

    /// Ubuntu's 6.8.0-136..147 backported an inverted reserved-field check for
    /// buffer rings (Launchpad #2162843): an all-zero `resv` is rejected with
    /// EINVAL. A correct kernel accepts the first attempt.
    pub fn register_pbuf_ring(&self, addr: *mut c_void, entries: u32, bgid: u16) -> bool {
        for attempt in 0..2 {
            let mut reg = BufReg {
                ring_addr: addr as u64,
                ring_entries: entries,
                bgid,
                flags: 0,
                resv: [0; 3],
            };
            if attempt == 1 {
                reg.resv[0] = 1;
            }
            let rc = unsafe {
                syscall(
                    SYS_REGISTER,
                    self.fd as i64,
                    IORING_REGISTER_PBUF_RING as i64,
                    &reg as *const BufReg as i64,
                    1,
                )
            };
            if rc == 0 {
                return true;
            }
        }
        false
    }

    #[inline]
    pub fn sq_space(&self) -> u32 {
        let head = unsafe { std::ptr::read_volatile(self.sq_head) };
        self.sq_entries.saturating_sub(self.cached_tail.wrapping_sub(head))
    }

    #[inline]
    pub fn sqe(&mut self) -> Option<*mut u8> {
        let head = unsafe { std::ptr::read_volatile(self.sq_head) };
        if self.cached_tail.wrapping_sub(head) >= self.sq_entries {
            return None;
        }
        let idx = self.cached_tail & self.sq_mask;
        self.cached_tail = self.cached_tail.wrapping_add(1);
        let p = unsafe { self.sqes.add(idx as usize * SQE_SIZE) };
        unsafe { std::ptr::write_bytes(p, 0, SQE_SIZE) };
        Some(p)
    }

    /// Publishes the SQEs written since the last call and enters the kernel.
    pub fn submit(&mut self, min_complete: u32, flags: u32) -> c_int {
        let tail = self.cached_tail;
        let to_submit = unsafe {
            let head = std::ptr::read_volatile(self.sq_head);
            let n = tail.wrapping_sub(head);
            let mut i = head;
            while i != tail {
                let slot = i & self.sq_mask;
                std::ptr::write_volatile(self.sq_array.add(slot as usize), slot);
                i = i.wrapping_add(1);
            }
            std::ptr::write_volatile(self.sq_tail, tail);
            n
        };
        let n = unsafe {
            syscall(
                SYS_ENTER,
                self.fd as i64,
                to_submit as i64,
                min_complete as i64,
                flags as i64,
                0,
                0,
            )
        };
        if n < 0 {
            return -1;
        }
        n as c_int
    }

    /// Reaps completions, calling `f(user_data, res, flags)` for each.
    pub fn reap<F: FnMut(u64, i32, u32)>(&self, mut f: F) -> usize {
        let head = unsafe { std::ptr::read_volatile(self.cq_head) };
        let tail = unsafe { std::ptr::read_volatile(self.cq_tail) };
        let mut i = head;
        while i != tail {
            let slot = (i & self.cq_mask) as usize;
            let p = unsafe { self.cqes.add(slot * CQE_SIZE) };
            let user = unsafe { std::ptr::read_volatile(p as *const u64) };
            let res = unsafe { std::ptr::read_volatile(p.add(8) as *const i32) };
            let flags = unsafe { std::ptr::read_volatile(p.add(12) as *const u32) };
            f(user, res, flags);
            i = i.wrapping_add(1);
        }
        if head != tail {
            unsafe { std::ptr::write_volatile(self.cq_head, tail) };
        }
        tail.wrapping_sub(head) as usize
    }

    pub fn pending(&self) -> u32 {
        let head = unsafe { std::ptr::read_volatile(self.cq_head) };
        let tail = unsafe { std::ptr::read_volatile(self.cq_tail) };
        tail.wrapping_sub(head)
    }

    pub fn enter(&self, min_complete: u32, flags: u32) -> c_int {
        let n = unsafe {
            syscall(
                SYS_ENTER,
                self.fd as i64,
                0,
                min_complete as i64,
                flags as i64,
                0,
                0,
            )
        };
        if n < 0 {
            -1
        } else {
            n as c_int
        }
    }
}

impl Drop for Ring {
    fn drop(&mut self) {
        unsafe {
            munmap(self.sqes_map, self.sqes_map_len);
            munmap(self.cq_map, self.cq_map_len);
            munmap(self.sq_map, self.sq_map_len);
            super::sys::close(self.fd);
        }
    }
}

// ---------------------------------------------------------------- SQE writing

pub struct Sqe(pub *mut u8);

impl Sqe {
    /// Applies a prepared request to this submission slot.
    #[inline]
    pub fn apply<F: FnOnce(&Sqe)>(self, f: F) {
        f(&self);
    }

    #[inline]
    fn put(&self, off: usize, v: u64) {
        unsafe { std::ptr::write_unaligned(self.0.add(off) as *mut u64, v) };
    }
    #[inline]
    fn put8(&self, off: usize, v: u8) {
        unsafe { std::ptr::write(self.0.add(off), v) };
    }
    #[inline]
    fn put16(&self, off: usize, v: u16) {
        unsafe { std::ptr::write_unaligned(self.0.add(off) as *mut u16, v) };
    }
    #[inline]
    fn put32(&self, off: usize, v: u32) {
        unsafe { std::ptr::write_unaligned(self.0.add(off) as *mut u32, v) };
    }

    pub fn nop(user: u64) -> impl FnOnce(&Sqe) {
        move |s: &Sqe| {
            s.put8(O_OPCODE, IORING_OP_NOP);
            s.put(O_USER, user);
        }
    }

    /// One-shot accept straight into a free slot of the ring's file table.
    /// Multishot accept is deliberately not used: a burst can hand the ring
    /// more connections than it has slots, and the overflow is accepted and
    /// then dropped with ENFILE. One-shot accepts are armed only while slots
    /// remain, so no connection is ever accepted that cannot be installed.
    pub fn accept(listen_fd: c_int, user: u64) -> impl FnOnce(&Sqe) {
        move |s: &Sqe| {
            s.put8(O_OPCODE, IORING_OP_ACCEPT);
            s.put32(O_FD, listen_fd as u32);
            s.put32(O_FILE_INDEX, IORING_FILE_INDEX_ALLOC as u32);
            s.put(O_USER, user);
        }
    }

    /// Hands `nbufs` buffers of `len` bytes, starting at id `bid`, to group
    /// `bgid`. The completion is skipped: nothing waits on it.
    ///
    /// `struct io_provide_buffers` maps onto the SQE as fd = count, addr = base,
    /// len = buffer length, off = first id, buf_group = group.
    pub fn provide_buffers(
        base: *const u8,
        len: u32,
        nbufs: u32,
        bgid: u16,
        bid: u32,
        user: u64,
    ) -> impl FnOnce(&Sqe) {
        move |s: &Sqe| {
            s.put8(O_OPCODE, IORING_OP_PROVIDE_BUFFERS);
            s.put8(O_FLAGS, IOSQE_CQE_SKIP_SUCCESS);
            s.put32(O_FD, nbufs);
            s.put(O_OFF, bid as u64);
            s.put(O_ADDR, base as u64);
            s.put32(O_LEN, len);
            s.put16(O_BUF_GROUP, bgid);
            s.put(O_USER, user);
        }
    }

    pub fn recv_multishot(file_index: u32, bgid: u16, user: u64) -> impl FnOnce(&Sqe) {
        move |s: &Sqe| {
            s.put8(O_OPCODE, IORING_OP_RECV_MULTISHOT);
            s.put8(O_FLAGS, IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT);
            s.put32(O_FD, file_index);
            s.put16(O_BUF_GROUP, bgid);
            s.put(O_USER, user);
        }
    }

    pub fn send(file_index: u32, ptr: *const u8, len: usize, user: u64) -> impl FnOnce(&Sqe) {
        move |s: &Sqe| {
            s.put8(O_OPCODE, IORING_OP_SEND);
            s.put8(O_FLAGS, IOSQE_FIXED_FILE);
            s.put32(O_FD, file_index);
            s.put(O_ADDR, ptr as u64);
            s.put32(O_LEN, len as u32);
            s.put32(O_FLAGS32, 0x4000); // MSG_NOSIGNAL
            s.put(O_USER, user);
        }
    }

    /// Shutdown both directions, hard-linked to the close that follows it. The
    /// armed multishot receive then completes (EOF or error) instead of pinning
    /// the socket open past the close.
    pub fn shutdown(file_index: u32, user: u64) -> impl FnOnce(&Sqe) {
        move |s: &Sqe| {
            s.put8(O_OPCODE, IORING_OP_SHUTDOWN);
            s.put8(O_FLAGS, IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK);
            s.put32(O_FD, file_index);
            s.put32(O_LEN, 2); // SHUT_RDWR
            s.put(O_USER, user);
        }
    }

    pub fn close(file_index: u32, user: u64) -> impl FnOnce(&Sqe) {
        move |s: &Sqe| {
            s.put8(O_OPCODE, IORING_OP_CLOSE);
            s.put8(O_FLAGS, IOSQE_FIXED_FILE);
            s.put32(O_FD, file_index);
            s.put(O_USER, user);
        }
    }
}

pub fn new_eventfd() -> c_int {
    // EFD_NONBLOCK | EFD_CLOEXEC
    unsafe { eventfd(0, 0x800 | 0x80000) }
}

pub fn read_eventfd(fd: c_int) {
    super::sys::drain_counter(fd);
}

#[cfg(test)]
mod abi_tests {
    use super::*;

    fn encode<F: FnOnce(&Sqe)>(f: F) -> [u8; SQE_SIZE] {
        let mut b = [0u8; SQE_SIZE];
        {
            let s = Sqe(b.as_mut_ptr());
            s.apply(f);
        }
        b
    }
    fn u32at(b: &[u8; SQE_SIZE], at: usize) -> u32 {
        u32::from_ne_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
    }
    fn u16at(b: &[u8; SQE_SIZE], at: usize) -> u16 {
        u16::from_ne_bytes([b[at], b[at + 1]])
    }
    fn u64at(b: &[u8; SQE_SIZE], at: usize) -> u64 {
        let mut a = [0u8; 8];
        a.copy_from_slice(&b[at..at + 8]);
        u64::from_ne_bytes(a)
    }

    /// One-shot direct accept: opcode, listener fd, allocating file index, and
    /// none of the fields the kernel rejects.
    #[test]
    fn accept_direct_layout() {
        let b = encode(Sqe::accept(7, 0x99));
        assert_eq!(b[O_OPCODE], IORING_OP_ACCEPT);
        assert_eq!(u32at(&b, O_FD), 7);
        assert_eq!(u32at(&b, O_FILE_INDEX), IORING_FILE_INDEX_ALLOC as u32);
        assert_eq!(u32at(&b, O_FLAGS32), 0, "multishot must stay off");
        assert_eq!(u64at(&b, O_OFF), 0, "addr2/off carries no address");
        assert_eq!(u32at(&b, O_LEN), 0, "accept rejects a length");
        assert_eq!(u16at(&b, O_BUF_GROUP), 0, "accept rejects a buffer group");
        assert_eq!(b[O_FLAGS], 0, "accept is not a fixed-file operation");
        assert_eq!(u64at(&b, O_USER), 0x99);
    }

    /// Provided buffers: count in fd, base in addr, length in len, first id in
    /// off, group in buf_group.
    #[test]
    fn provide_buffers_layout() {
        let base = 0x1000usize as *const u8;
        let b = encode(Sqe::provide_buffers(base, 2048, 32, 0, 4096, 5));
        assert_eq!(b[O_OPCODE], IORING_OP_PROVIDE_BUFFERS);
        assert_eq!(b[O_FLAGS], IOSQE_CQE_SKIP_SUCCESS);
        assert_eq!(u32at(&b, O_FD), 32, "buffer count");
        assert_eq!(u64at(&b, O_ADDR), 0x1000);
        assert_eq!(u32at(&b, O_LEN), 2048);
        assert_eq!(u64at(&b, O_OFF), 4096, "first buffer id");
        assert_eq!(u16at(&b, O_BUF_GROUP), 0);
    }

    /// Multishot receive on a fixed descriptor with buffer selection.
    #[test]
    fn recv_multishot_layout() {
        let b = encode(Sqe::recv_multishot(1234, 0, 6));
        assert_eq!(b[O_OPCODE], IORING_OP_RECV_MULTISHOT);
        assert_eq!(b[O_FLAGS], IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT);
        assert_eq!(u32at(&b, O_FD), 1234);
        assert_eq!(u16at(&b, O_BUF_GROUP), 0);
        assert_eq!(u32at(&b, O_LEN), 0);
    }

    #[test]
    fn send_layout() {
        let buf = b"x".as_ptr();
        let b = encode(Sqe::send(9, buf, 41, 7));
        assert_eq!(b[O_OPCODE], IORING_OP_SEND);
        assert_eq!(b[O_FLAGS], IOSQE_FIXED_FILE);
        assert_eq!(u32at(&b, O_FD), 9);
        assert_eq!(u32at(&b, O_LEN), 41);
        assert_eq!(u32at(&b, O_FLAGS32), 0x4000, "MSG_NOSIGNAL");
        assert_eq!(u64at(&b, O_ADDR), buf as u64);
    }

    /// The close is hard-linked behind the shutdown so the armed receive
    /// terminates instead of pinning the socket.
    #[test]
    fn shutdown_links_the_close() {
        let b = encode(Sqe::shutdown(11, 3));
        assert_eq!(b[O_OPCODE], IORING_OP_SHUTDOWN);
        assert_eq!(b[O_FLAGS], IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK);
        assert_eq!(u32at(&b, O_LEN), 2, "SHUT_RDWR");
        let c = encode(Sqe::close(11, 4));
        assert_eq!(c[O_OPCODE], IORING_OP_CLOSE);
        assert_eq!(c[O_FLAGS], IOSQE_FIXED_FILE);
        assert_eq!(u32at(&c, O_FD), 11);
    }

    #[test]
    fn struct_sizes_match_the_uapi() {
        assert_eq!(std::mem::size_of::<Params>(), 120);
        assert_eq!(std::mem::size_of::<SqOffsets>(), 40);
        assert_eq!(std::mem::size_of::<CqOffsets>(), 40);
        assert_eq!(std::mem::size_of::<BufReg>(), 40);
        assert_eq!(std::mem::size_of::<BufEntry>(), 16);
        assert_eq!(SQE_SIZE, 64);
        assert_eq!(CQE_SIZE, 16);
    }
}
