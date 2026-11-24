use std::ffi::{c_int, c_long, c_uint, c_void};

use crate::sys::syscall;

#[repr(C)]
#[derive(Default)]
pub struct io_sqring_offsets {
    pub head: u32,
    pub tail: u32,
    pub ring_mask: u32,
    pub ring_entries: u32,
    pub flags: u32,
    pub dropped: u32,
    pub array: u32,
    pub resv1: u32,
    pub user_addr: u64,
}

#[repr(C)]
#[derive(Default)]
pub struct io_cqring_offsets {
    pub head: u32,
    pub tail: u32,
    pub ring_mask: u32,
    pub ring_entries: u32,
    pub overflow: u32,
    pub cqes: u32,
    pub flags: u32,
    pub resv1: u32,
    pub user_addr: u64,
}

#[repr(C)]
#[derive(Default)]
pub struct io_uring_params {
    pub sq_entries: u32,
    pub cq_entries: u32,
    pub flags: u32,
    pub sq_thread_cpu: u32,
    pub sq_thread_idle: u32,
    pub features: u32,
    pub wq_fd: u32,
    pub resv: [u32; 3],
    pub sq_off: io_sqring_offsets,
    pub cq_off: io_cqring_offsets,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub union off_or_addr2 {
    pub off: u64,
    pub addr2: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub union addr_or_splice_off_in {
    pub addr: u64,
    pub splice_off_in: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub union op_flags {
    pub rw_flags: i32,
    pub fsync_flags: u32,
    pub poll_events: u16,
    pub poll32_events: u32,
    pub timeout_flags: u32,
    pub accept_flags: u32,
    pub cancel_flags: u32,
    pub open_flags: u32,
    pub msg_flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub union buf_index_or_group {
    pub buf_index: u16,
    pub buf_group: u16,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub union splice_fd_in_or_file_index {
    pub splice_fd_in: i32,
    pub file_index: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct io_uring_sqe {
    pub opcode: u8,
    pub flags: u8,
    pub ioprio: u16,
    pub fd: i32,
    pub off_or_addr2: off_or_addr2,
    pub addr_or_splice_off_in: addr_or_splice_off_in,
    pub len: u32,
    pub op_flags: op_flags,
    pub user_data: u64,
    pub buf_index_or_group: buf_index_or_group,
    pub personality: u16,
    pub splice_fd_in_or_file_index: splice_fd_in_or_file_index,
    pub addr3: u64,
    pub __pad2: [u64; 1],
}

impl Default for io_uring_sqe {
    fn default() -> Self {
        // SAFETY: every field is an integer or a union of integers, so all zeroes is valid.
        unsafe { std::mem::zeroed() }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct io_uring_cqe {
    pub user_data: u64,
    pub res: i32,
    pub flags: u32,
}

#[repr(C)]
pub struct io_uring_rsrc_register {
    pub nr: u32,
    pub flags: u32,
    pub resv2: u64,
    pub data: u64,
    pub tags: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct io_uring_buf_reg {
    pub ring_addr: u64,
    pub ring_entries: u32,
    pub bgid: u16,
    pub flags: u16,
    pub min_left: u32,
    pub resv: [u32; 5],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct io_uring_buf {
    pub addr: u64,
    pub len: u32,
    pub bid: u16,
    pub resv: u16,
}

// syscalls
pub const SYS_IO_URING_SETUP: c_long = 425;
pub const SYS_IO_URING_ENTER: c_long = 426;
pub const SYS_IO_URING_REGISTER: c_long = 427;

// io_uring_setup flags
pub const IORING_SETUP_CQSIZE: u32 = 1 << 3;
pub const IORING_SETUP_NO_SQARRAY: u32 = 1 << 16;

// sq ring flags
pub const IORING_SQ_CQ_OVERFLOW: u32 = 1 << 1;

// io_uring_params.features
pub const IORING_FEAT_SINGLE_MMAP: u32 = 1 << 0;

// mmap offsets
pub const IORING_OFF_SQ_RING: i64 = 0;
pub const IORING_OFF_SQES: i64 = 0x10000000;

// io_uring_enter flags
pub const IORING_ENTER_GETEVENTS: u32 = 1 << 0;

// io_uring_register opcodes and flags
pub const IORING_REGISTER_EVENTFD: c_uint = 4;
pub const IORING_REGISTER_FILES2: c_uint = 13;
pub const IORING_RSRC_REGISTER_SPARSE: u32 = 1 << 0;

pub const IORING_REGISTER_PBUF_RING: c_uint = 22;

// sqe.opcode
pub const IORING_OP_POLL_ADD: u8 = 6;
pub const IORING_OP_ACCEPT: u8 = 13;
pub const IORING_OP_ASYNC_CANCEL: u8 = 14;
pub const IORING_OP_CLOSE: u8 = 19;
pub const IORING_OP_SEND: u8 = 26;
pub const IORING_OP_RECV: u8 = 27;

// sqe.flags
pub const IOSQE_FIXED_FILE: u8 = 1 << 0;
pub const IOSQE_BUFFER_SELECT: u8 = 1 << 5;

// sqe.ioprio for accept
pub const IORING_ACCEPT_MULTISHOT: u16 = 1 << 0;

// sqe.ioprio for recv
pub const IORING_RECV_MULTISHOT: u16 = 1 << 1;

// poll_add: sqe.len, sqe.poll32_events
pub const IORING_POLL_ADD_MULTI: u32 = 1 << 0;
pub const POLLIN: u32 = 0x0001;

// sqe.file_index
pub const IORING_FILE_INDEX_ALLOC: u32 = u32::MAX;

// cqe.flags
pub const IORING_CQE_F_MORE: u32 = 1 << 1;
pub const IORING_CQE_F_BUFFER: u32 = 1 << 0;
pub const IORING_CQE_BUFFER_SHIFT: u32 = 16;

// errno
pub const EINVAL: i32 = 22;
pub const ENOBUFS: i32 = 105;

pub unsafe fn io_uring_setup(entries: u32, p: *mut io_uring_params) -> c_int {
    unsafe { syscall(SYS_IO_URING_SETUP, entries as c_long, p) as c_int }
}

pub unsafe fn io_uring_enter(
    fd: c_int,
    to_submit: u32,
    min_complete: u32,
    flags: u32,
    arg: *const c_void,
    argsz: usize,
) -> c_int {
    unsafe {
        syscall(
            SYS_IO_URING_ENTER,
            fd as c_long,
            to_submit as c_long,
            min_complete as c_long,
            flags as c_long,
            arg,
            argsz as c_long,
        ) as c_int
    }
}

pub unsafe fn io_uring_register(
    fd: c_int,
    opcode: c_uint,
    arg: *const c_void,
    nr_args: c_uint,
) -> c_int {
    unsafe { syscall(SYS_IO_URING_REGISTER, fd, opcode, arg, nr_args) as c_int }
}
