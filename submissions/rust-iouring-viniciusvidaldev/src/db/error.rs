use std::{
    ffi::{CStr, c_int},
    fmt, io,
};

use crate::sys::sqlite;

#[derive(Debug)]
pub struct Error {
    pub(super) code: c_int,
    pub(super) msg: String,
}

impl Error {
    /// A statement that must yield a row finished without one.
    pub(super) fn no_row(msg: &str) -> Self {
        Self {
            code: sqlite::SQLITE_INTERNAL,
            msg: msg.into(),
        }
    }

    pub(super) fn from_db(db: *mut sqlite::sqlite3, code: c_int) -> Self {
        let msg = unsafe { CStr::from_ptr(sqlite::sqlite3_errmsg(db)) };
        Self {
            code,
            msg: msg.to_string_lossy().into_owned(),
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "sqlite error {}: {}", self.code, self.msg)
    }
}

impl std::error::Error for Error {}

impl From<Error> for io::Error {
    fn from(e: Error) -> Self {
        io::Error::other(e)
    }
}
