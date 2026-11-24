use std::{ffi::c_int, ptr::NonNull};

use crate::{db::error::Error, sys::sqlite};

pub(super) struct Stmt {
    raw: NonNull<sqlite::sqlite3_stmt>,
}

impl Stmt {
    pub(super) fn from_raw(raw: *mut sqlite::sqlite3_stmt) -> Option<Self> {
        NonNull::new(raw).map(|raw| Self { raw })
    }

    pub(super) fn query(&mut self) -> Query<'_> {
        Query { stmt: self }
    }

    fn raw(&self) -> *mut sqlite::sqlite3_stmt {
        self.raw.as_ptr()
    }

    fn error(&self, code: c_int) -> Error {
        let db = unsafe { sqlite::sqlite3_db_handle(self.raw()) };
        Error::from_db(db, code)
    }
}

impl Drop for Stmt {
    fn drop(&mut self) {
        unsafe { sqlite::sqlite3_finalize(self.raw()) };
    }
}

pub(super) struct Query<'q> {
    stmt: &'q mut Stmt,
}

impl<'q> Query<'q> {
    fn check(&self, code: c_int) -> Result<(), Error> {
        if code != sqlite::SQLITE_OK {
            return Err(self.stmt.error(code));
        }
        Ok(())
    }

    pub(super) fn bind_i64(&mut self, idx: c_int, value: i64) -> Result<(), Error> {
        let code = unsafe { sqlite::sqlite3_bind_int64(self.stmt.raw(), idx, value) };
        self.check(code)
    }

    // SQLITE_STATIC: sqlite keeps the pointer after Drop, but only reads it while stepping, and every
    // query rebinds all its parameters before stepping
    pub(super) fn bind_text(&mut self, idx: c_int, value: &'q [u8]) -> Result<(), Error> {
        let Ok(len) = c_int::try_from(value.len()) else {
            return Err(Error {
                code: sqlite::SQLITE_TOOBIG,
                msg: "text too large to bind".into(),
            });
        };

        let code = unsafe {
            sqlite::sqlite3_bind_text(
                self.stmt.raw(),
                idx,
                value.as_ptr().cast(),
                len,
                sqlite::SQLITE_STATIC,
            )
        };
        self.check(code)
    }

    pub(super) fn step(&mut self) -> Result<Step<'_>, Error> {
        match unsafe { sqlite::sqlite3_step(self.stmt.raw()) } {
            sqlite::SQLITE_ROW => Ok(Step::Row(Row { stmt: self.stmt })),
            sqlite::SQLITE_DONE => Ok(Step::Done),
            code => Err(self.stmt.error(code)),
        }
    }

    pub(super) fn changes(&self) -> i64 {
        unsafe { sqlite::sqlite3_changes64(sqlite::sqlite3_db_handle(self.stmt.raw())) }
    }
}

impl Drop for Query<'_> {
    fn drop(&mut self) {
        // no sqlite3_clear_bindings: it expires statements whose plan used a bound value
        // (posts by_id), so sqlite re-parses them on the next step
        unsafe { sqlite::sqlite3_reset(self.stmt.raw()) };
    }
}

pub(super) enum Step<'q> {
    Row(Row<'q>),
    Done,
}

pub(super) struct Row<'r> {
    stmt: &'r Stmt,
}

impl<'r> Row<'r> {
    pub(super) fn i64(&self, col: c_int) -> i64 {
        unsafe { sqlite::sqlite3_column_int64(self.stmt.raw(), col) }
    }

    pub(super) fn text(&self, col: c_int) -> &'r [u8] {
        // column_text must be called before column_bytes
        let ptr = unsafe { sqlite::sqlite3_column_text(self.stmt.raw(), col) };
        if ptr.is_null() {
            return &[];
        }
        let len = unsafe { sqlite::sqlite3_column_bytes(self.stmt.raw(), col) };
        unsafe { std::slice::from_raw_parts(ptr, len as usize) }
    }
}
