pub mod eventfd;
pub mod io_uring;
pub mod mmap;
pub mod resource;
pub mod socket;
pub mod sqlite;

use std::{
    ffi::{c_int, c_long},
    io,
};

unsafe extern "C" {
    pub fn syscall(number: c_long, ...) -> c_long;
}

pub fn cvt(ret: c_int) -> io::Result<c_int> {
    if ret == -1 {
        return Err(io::Error::last_os_error());
    }
    Ok(ret)
}
