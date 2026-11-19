//! Small FFI boundary for the mapped PBUF_RING multi-ring L2 transport.
//!
//! All ring access is synchronous and single-owner; callers must not wait
//! while an App batch is open. Operation user_data is an owned operation-table
//! key, never a connection descriptor or slot by itself.

use std::ffi::c_void;
use std::io;
use std::ptr::NonNull;

#[repr(C)]
#[derive(Clone, Copy)]
struct RawCompletion {
    pub user_data: u64,
    pub result: i32,
    pub flags: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct Completion {
    pub user_data: u64,
    pub result: i32,
    pub flags: u32,
    pub buffer_id: Option<u16>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum BufferState {
    Kernel,
    User,
}

pub enum RingOpaque {}

unsafe extern "C" {
    fn l2_ring_create(
        entries: u32,
        cq_entries: u32,
        file_slots: u32,
        buffer_group: u16,
        buffer_count: u32,
        error_out: *mut i32,
    ) -> *mut RingOpaque;
    fn l2_ring_event_fd(ring: *const RingOpaque) -> i32;
    fn l2_ring_destroy(ring: *mut RingOpaque);
    fn l2_submit(ring: *mut RingOpaque) -> i32;
    fn l2_prep_accept(ring: *mut RingOpaque, listener: i32, slot: u32, user_data: u64) -> i32;
    fn l2_prep_recv_select(ring: *mut RingOpaque, slot: u32, user_data: u64) -> i32;
    fn l2_return_buffer(ring: *mut RingOpaque, id: u32) -> i32;
    fn l2_buffer_ptr(ring: *mut RingOpaque, id: u32) -> *mut u8;
    fn l2_buffer_size() -> u32;
    fn l2_ring_buffer_count(ring: *const RingOpaque) -> u32;
    fn l2_cq_ready(ring: *mut RingOpaque) -> u32;
    fn l2_prep_send(
        ring: *mut RingOpaque,
        slot: u32,
        buffer: *const c_void,
        length: u32,
        user_data: u64,
    ) -> i32;
    fn l2_prep_close(ring: *mut RingOpaque, slot: u32, user_data: u64) -> i32;
    fn l2_peek_cqe(ring: *mut RingOpaque, out: *mut RawCompletion) -> i32;
    fn l2_wait_cqe(ring: *mut RingOpaque, out: *mut RawCompletion) -> i32;
    fn l2_wait_many(rings: *const *mut RingOpaque, count: u32, timeout_ms: i32, ready_mask: *mut u64) -> i32;
}

/// One ring is created and used by exactly one issuer thread.
pub struct Ring {
    ptr: NonNull<RingOpaque>,
    buffer_states: Vec<BufferState>,
    available_buffers: usize,
    buffer_size: usize,
}

impl Ring {
    pub fn new(
        entries: u32,
        cq_entries: u32,
        file_slots: u32,
        buffer_group: u16,
        buffer_count: u32,
    ) -> io::Result<Self> {
        let mut error = 0;
        let raw = unsafe {
            l2_ring_create(entries, cq_entries, file_slots, buffer_group, buffer_count, &mut error)
        };
        let ptr = NonNull::new(raw).ok_or_else(|| io::Error::from_raw_os_error(-error))?;
        let count = unsafe { l2_ring_buffer_count(ptr.as_ptr()) as usize };
        let size = unsafe { l2_buffer_size() as usize };
        if count == 0 || size == 0 {
            unsafe { l2_ring_destroy(ptr.as_ptr()) };
            return Err(io::Error::new(io::ErrorKind::InvalidData, "invalid provided-buffer geometry"));
        }
        Ok(Self {
            ptr,
            buffer_states: vec![BufferState::Kernel; count],
            available_buffers: count,
            buffer_size: size,
        })
    }

    #[inline]
    fn raw(&mut self) -> *mut RingOpaque {
        self.ptr.as_ptr()
    }

    #[inline]
    pub fn raw_handle(&self) -> *mut RingOpaque {
        self.ptr.as_ptr()
    }

    pub fn event_fd(&self) -> i32 {
        unsafe { l2_ring_event_fd(self.ptr.as_ptr()) }
    }

    /// Prepare one direct accept SQE. A zero return means it is staged.
    ///
    /// # Safety
    /// Keep `listener` open until the terminal CQE is consumed. Reserve `slot`
    /// for this accept and do not reuse it until its direct-close CQE completes.
    pub unsafe fn accept(&mut self, listener: i32, slot: u32, key: u64) -> io::Result<()> {
        cvt_zero(unsafe { l2_prep_accept(self.raw(), listener, slot, key) })
    }

    /// Receive into the ring's mapped buffer ring. The selected buffer becomes
    /// user-owned only when its CQE is consumed.
    ///
    /// # Safety
    /// `slot` must own a live direct descriptor. Keep its generation reserved
    /// until the receive CQE (and any cancellation race) is terminal.
    pub unsafe fn recv_select(&mut self, slot: u32, key: u64) -> io::Result<()> {
        cvt_zero(unsafe { l2_prep_recv_select(self.raw(), slot, key) })
    }

    /// Return a raw read pointer and byte capacity to a user-owned provided buffer.
    #[inline]
    pub fn buffer_ptr_and_len(&self, id: u16) -> io::Result<(*const u8, usize)> {
        if self.buffer_states.get(id as usize) != Some(&BufferState::User) {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "provided buffer is not user-owned"));
        }
        let ptr = unsafe { l2_buffer_ptr(self.ptr.as_ptr(), id as u32) };
        if ptr.is_null() {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "invalid provided buffer id"));
        }
        Ok((ptr as *const u8, self.buffer_size))
    }

    /// Access a receive buffer after its CQE transferred it to userspace.
    pub fn provided_buffer(&mut self, id: u16) -> io::Result<&mut [u8]> {
        if self.buffer_states.get(id as usize) != Some(&BufferState::User) {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "provided buffer is not user-owned"));
        }
        let ptr = unsafe { l2_buffer_ptr(self.raw(), id as u32) };
        if ptr.is_null() { return Err(io::ErrorKind::InvalidInput.into()); }
        Ok(unsafe { std::slice::from_raw_parts_mut(ptr, self.buffer_size) })
    }

    /// Number of buffers currently owned by the kernel in O(1).
    #[inline]
    pub fn available_buffer_count(&self) -> usize {
        self.available_buffers
    }

    pub fn buffer_ownership_counts(&self) -> (usize, usize) {
        let kernel = self.available_buffers;
        let user = self.buffer_states.len().saturating_sub(kernel);
        (kernel, user)
    }

    /// Synchronously return a consumed buffer to the mapped PBUF_RING (zero SQEs).
    #[inline]
    pub fn return_buffer(&mut self, id: u16) -> io::Result<()> {
        let idx = id as usize;
        if self.buffer_states.get(idx) != Some(&BufferState::User) {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "buffer is not user-owned"));
        }
        cvt_zero(unsafe { l2_return_buffer(self.raw(), id as u32) })?;
        self.buffer_states[idx] = BufferState::Kernel;
        self.available_buffers += 1;
        Ok(())
    }

    /// Count of CQEs currently visible without blocking.
    #[inline]
    pub fn cq_ready(&mut self) -> u32 {
        unsafe { l2_cq_ready(self.raw()) }
    }

    /// Prepare a fixed-file send.
    ///
    /// # Safety
    /// `buffer` must point to `length` initialized bytes that remain allocated
    /// and immutable until the terminal CQE for `key` is consumed.
    pub unsafe fn send(&mut self, slot: u32, buffer: *const u8, length: u32, key: u64) -> io::Result<()> {
        cvt_zero(unsafe { l2_prep_send(self.raw(), slot, buffer.cast(), length, key) })
    }

    /// # Safety
    /// `slot` must own a live descriptor with no outstanding operation that
    /// can still use it. Do not reuse the slot until this close's CQE succeeds.
    pub unsafe fn close(&mut self, slot: u32, key: u64) -> io::Result<()> {
        cvt_zero(unsafe { l2_prep_close(self.raw(), slot, key) })
    }

    /// Submit all staged SQEs. The result is the count accepted by the kernel.
    pub fn submit(&mut self) -> io::Result<u32> {
        cvt_count(unsafe { l2_submit(self.raw()) })
    }

    /// Return one already available completion without blocking.
    #[inline]
    pub fn peek(&mut self) -> io::Result<Option<Completion>> {
        loop {
            let mut cqe = RawCompletion { user_data: 0, result: 0, flags: 0 };
            let rc = unsafe { l2_peek_cqe(self.raw(), &mut cqe) };
            if rc == 0 { return Ok(None); }
            if rc == 1 { return self.transfer_completion(cqe).map(Some); }
            let error = io::Error::from_raw_os_error(-rc);
            if error.kind() == io::ErrorKind::Interrupted { continue; }
            if error.kind() == io::ErrorKind::WouldBlock { return Ok(None); }
            return Err(error);
        }
    }

    /// Block for one completion. Call only after all application batches close.
    pub fn wait(&mut self) -> io::Result<Completion> {
        let mut cqe = RawCompletion { user_data: 0, result: 0, flags: 0 };
        let rc = unsafe { l2_wait_cqe(self.raw(), &mut cqe) };
        if rc == 0 { self.transfer_completion(cqe) } else { Err(io::Error::from_raw_os_error(-rc)) }
    }

    /// Poll a set of ring eventfds via raw opaque ring handles and drain notified counters.
    pub fn wait_any_raw(ptrs: &[*mut RingOpaque], timeout_ms: i32) -> io::Result<u64> {
        if ptrs.is_empty() || ptrs.len() > 64 {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "ring count must be 1..=64"));
        }
        let mut ready_mask = 0u64;
        let rc = unsafe { l2_wait_many(ptrs.as_ptr(), ptrs.len() as u32, timeout_ms, &mut ready_mask) };
        if rc < 0 { Err(io::Error::from_raw_os_error(-rc)) } else { Ok(ready_mask) }
    }

    /// Poll a slice of rings and drain each notified eventfd counter.
    pub fn wait_any(rings: &mut [Ring], timeout_ms: i32) -> io::Result<u64> {
        if rings.is_empty() || rings.len() > 64 {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "ring count must be 1..=64"));
        }
        let mut pointers = [std::ptr::null_mut(); 64];
        for (dst, ring) in pointers.iter_mut().zip(rings.iter()) {
            *dst = ring.ptr.as_ptr();
        }
        Self::wait_any_raw(&pointers[..rings.len()], timeout_ms)
    }

    #[inline]
    fn transfer_completion(&mut self, raw: RawCompletion) -> io::Result<Completion> {
        const CQE_F_BUFFER: u32 = 1;
        const CQE_BUFFER_SHIFT: u32 = 16;
        let buffer_id = if raw.flags & CQE_F_BUFFER != 0 {
            let id = ((raw.flags >> CQE_BUFFER_SHIFT) & 0xffff) as u16;
            let Some(state) = self.buffer_states.get_mut(id as usize) else {
                return Err(io::Error::new(io::ErrorKind::InvalidData, "kernel returned an unknown buffer id"));
            };
            if *state != BufferState::Kernel {
                return Err(io::Error::new(io::ErrorKind::InvalidData, "kernel returned a buffer not owned by the pool"));
            }
            *state = BufferState::User;
            self.available_buffers -= 1;
            Some(id)
        } else {
            None
        };
        Ok(Completion { user_data: raw.user_data, result: raw.result, flags: raw.flags, buffer_id })
    }
}

impl Drop for Ring {
    fn drop(&mut self) {
        unsafe { l2_ring_destroy(self.ptr.as_ptr()) }
    }
}

fn cvt_zero(rc: i32) -> io::Result<()> {
    if rc == 0 { Ok(()) } else { Err(io::Error::from_raw_os_error(-rc)) }
}

fn cvt_count(rc: i32) -> io::Result<u32> {
    if rc >= 0 { Ok(rc as u32) } else { Err(io::Error::from_raw_os_error(-rc)) }
}

/// Startup probe validating kernel io_uring setup, sparse direct file tables,
/// and mapped PBUF_RING registration before opening SQLite or worker forks.
pub fn self_check() -> bool {
    Ring::new(64, 128, 64, 1, 8).is_ok()
}
