mod health;
mod likes;
mod posts;

pub use health::Health;
pub use likes::Likes;
pub use posts::{Post, Posts};

use super::{conn::Connection, error::Error};

pub enum Next<T> {
    Row(T),
    Done,
}

pub enum Insert {
    Inserted,
    Duplicate,
    MissingParent,
}

pub(super) struct Stmts {
    pub(super) health: Health,
    pub(super) likes: Likes,
    pub(super) posts: Posts,
}

impl Stmts {
    pub(super) fn prepare(conn: &Connection) -> Result<Self, Error> {
        Ok(Self {
            health: Health::prepare(conn)?,
            likes: Likes::prepare(conn)?,
            posts: Posts::prepare(conn)?,
        })
    }
}
