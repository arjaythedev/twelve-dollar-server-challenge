use std::{
    ffi::{CStr, c_int},
    ptr::{self, NonNull},
};

use super::{error::Error, stmt::Stmt};
use crate::sys::sqlite;

pub(super) struct Connection {
    raw: NonNull<sqlite::sqlite3>,
}

impl Connection {
    pub(super) fn open(path: &CStr) -> Result<Self, Error> {
        let mut raw = ptr::null_mut();
        let flags = sqlite::SQLITE_OPEN_READWRITE | sqlite::SQLITE_OPEN_NOMUTEX;
        let code = unsafe { sqlite::sqlite3_open_v2(path.as_ptr(), &mut raw, flags, ptr::null()) };

        let Some(raw) = NonNull::new(raw) else {
            return Err(Error {
                code,
                msg: "out of memory opening database".into(),
            });
        };

        let conn = Self { raw };
        conn.check(code)?;

        unsafe { sqlite::sqlite3_extended_result_codes(conn.raw(), 1) };
        Ok(conn)
    }

    pub(super) fn exec(&self, sql: &CStr) -> Result<(), Error> {
        let code = unsafe {
            sqlite::sqlite3_exec(
                self.raw(),
                sql.as_ptr(),
                None,
                ptr::null_mut(),
                ptr::null_mut(),
            )
        };
        self.check(code)
    }

    pub(super) fn prepare(&self, sql: &str) -> Result<Stmt, Error> {
        let mut raw = ptr::null_mut();
        let code = unsafe {
            sqlite::sqlite3_prepare_v3(
                self.raw(),
                sql.as_ptr().cast(),
                sql.len() as c_int,
                sqlite::SQLITE_PREPARE_PERSISTENT,
                &mut raw,
                ptr::null_mut(),
            )
        };
        self.check(code)?;

        let Some(stmt) = Stmt::from_raw(raw) else {
            return Err(Error {
                code: sqlite::SQLITE_MISUSE,
                msg: "sql contains no statement".into(),
            });
        };
        Ok(stmt)
    }

    fn raw(&self) -> *mut sqlite::sqlite3 {
        self.raw.as_ptr()
    }

    fn check(&self, code: c_int) -> Result<(), Error> {
        if code != sqlite::SQLITE_OK {
            return Err(Error::from_db(self.raw(), code));
        }
        Ok(())
    }
}

impl Drop for Connection {
    fn drop(&mut self) {
        unsafe { sqlite::sqlite3_close_v2(self.raw()) };
    }
}
