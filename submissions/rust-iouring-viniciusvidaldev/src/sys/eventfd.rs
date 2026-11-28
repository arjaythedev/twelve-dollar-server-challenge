use std::ffi::{c_int, c_uint};

pub const EFD_NONBLOCK: c_int = 0o4000;
pub const EFD_CLOEXEC: c_int = 0o2000000;

unsafe extern "C" {
    pub fn eventfd(initval: c_uint, flags: c_int) -> c_int;
}
