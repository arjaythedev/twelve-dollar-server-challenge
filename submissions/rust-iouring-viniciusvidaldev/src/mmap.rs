use std::{
    io,
    os::fd::{AsRawFd, BorrowedFd},
    ptr::{self, NonNull},
};

use crate::sys::mmap;

pub struct Mmap {
    ptr: NonNull<u8>,
    len: usize,
}

impl Mmap {
    pub fn new(fd: BorrowedFd<'_>, len: usize, offset: i64) -> io::Result<Self> {
        let raw = unsafe {
            mmap::mmap(
                ptr::null_mut(),
                len,
                mmap::PROT_READ | mmap::PROT_WRITE,
                mmap::MAP_SHARED | mmap::MAP_POPULATE,
                fd.as_raw_fd(),
                offset,
            )
        };
        if raw == mmap::MAP_FAILED {
            return Err(io::Error::last_os_error());
        }
        let ptr = NonNull::new(raw.cast()).ok_or_else(|| io::Error::other("mmap returned null"))?;
        Ok(Self { ptr, len })
    }

    pub fn anon(len: usize) -> io::Result<Self> {
        let raw = unsafe {
            mmap::mmap(
                ptr::null_mut(),
                len,
                mmap::PROT_READ | mmap::PROT_WRITE,
                mmap::MAP_PRIVATE | mmap::MAP_ANONYMOUS | mmap::MAP_POPULATE,
                -1,
                0,
            )
        };
        if raw == mmap::MAP_FAILED {
            return Err(io::Error::last_os_error());
        }
        let ptr = NonNull::new(raw.cast()).ok_or_else(|| io::Error::other("mmap returned null"))?;
        Ok(Self { ptr, len })
    }

    pub fn as_ptr(&self) -> *mut u8 {
        self.ptr.as_ptr()
    }
}

impl Drop for Mmap {
    fn drop(&mut self) {
        unsafe { mmap::munmap(self.as_ptr().cast(), self.len) };
    }
}
