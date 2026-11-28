use std::{
    io,
    os::fd::{FromRawFd, OwnedFd},
};

use crate::sys::{cvt, eventfd};

pub fn new_eventfd() -> io::Result<OwnedFd> {
    let res = unsafe { eventfd::eventfd(0, eventfd::EFD_NONBLOCK | eventfd::EFD_CLOEXEC) };
    let fd = cvt(res)?;
    Ok(unsafe { OwnedFd::from_raw_fd(fd) })
}
