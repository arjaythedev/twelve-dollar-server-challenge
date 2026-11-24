use crate::db::{
    conn::Connection,
    error::Error,
    stmt::{Step, Stmt},
};

mod ping {
    pub const SQL: &str = "SELECT 1";
}

pub struct Health {
    ping: Stmt,
}

impl Health {
    pub(super) fn prepare(conn: &Connection) -> Result<Self, Error> {
        Ok(Self {
            ping: conn.prepare(ping::SQL)?,
        })
    }

    pub fn ping(&mut self) -> Result<(), Error> {
        let mut query = self.ping.query();

        match query.step()? {
            Step::Row(_) => Ok(()),
            Step::Done => Err(Error::no_row("SELECT 1 returned no row")),
        }
    }
}
