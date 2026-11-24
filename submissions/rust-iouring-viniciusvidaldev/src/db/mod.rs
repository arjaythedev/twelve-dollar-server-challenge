mod conn;
mod error;
mod queries;
mod stmt;

use std::ffi::CStr;

pub use error::Error;
pub use queries::{Health, Insert, Likes, Next, Post, Posts};

use crate::db::{conn::Connection, queries::Stmts};

pub struct Db {
    // stmts must be finalized before conn closes
    stmts: Stmts,
    _conn: Connection,
}

impl Db {
    pub fn open(path: &CStr) -> Result<Self, Error> {
        let conn = Connection::open(path)?;
        conn.exec(c"PRAGMA journal_mode = WAL")?;
        conn.exec(c"PRAGMA synchronous = NORMAL")?;
        // Likes::add relies on FK violations to detect a missing post
        conn.exec(c"PRAGMA foreign_keys = ON")?;

        let stmts = Stmts::prepare(&conn)?;
        Ok(Self { stmts, _conn: conn })
    }

    pub fn health(&mut self) -> &mut Health {
        &mut self.stmts.health
    }

    pub fn likes(&mut self) -> &mut Likes {
        &mut self.stmts.likes
    }

    pub fn posts(&mut self) -> &mut Posts {
        &mut self.stmts.posts
    }
}
