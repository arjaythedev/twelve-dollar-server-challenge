use std::io;

use crate::sys::{cvt, resource};

/// Raises the soft fd limit to the hard limit and returns the new soft limit.
pub fn raise_nofile() -> io::Result<u64> {
    let mut lim = resource::rlimit {
        rlim_cur: 0,
        rlim_max: 0,
    };
    cvt(unsafe { resource::getrlimit(resource::RLIMIT_NOFILE, &mut lim) })?;
    lim.rlim_cur = lim.rlim_max;
    cvt(unsafe { resource::setrlimit(resource::RLIMIT_NOFILE, &lim) })?;
    Ok(lim.rlim_cur)
}
