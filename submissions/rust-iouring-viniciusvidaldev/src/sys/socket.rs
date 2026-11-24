use std::ffi::c_int;

unsafe extern "C" {
    pub fn listen(fd: c_int, backlog: c_int) -> c_int;
}
