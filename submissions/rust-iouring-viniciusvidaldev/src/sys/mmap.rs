use std::ffi::{c_int, c_void};

pub const PROT_READ: c_int = 0x1;
pub const PROT_WRITE: c_int = 0x2;

pub const MAP_SHARED: c_int = 0x01;
pub const MAP_PRIVATE: c_int = 0x02;
pub const MAP_ANONYMOUS: c_int = 0x20;
pub const MAP_POPULATE: c_int = 0x8000;

pub const MAP_FAILED: *mut c_void = !0usize as *mut c_void;

unsafe extern "C" {
    pub fn mmap(
        addr: *mut c_void,
        length: usize,
        prot: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    pub fn munmap(addr: *mut c_void, length: usize) -> c_int;
}
