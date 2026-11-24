use crate::{
    app::{PostId, UserId},
    db::{conn::Connection, error::Error, queries::Insert, stmt::Stmt},
    sys::sqlite,
};

mod insert {
    pub const SQL: &str = "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING";
    pub const USER_ID: i32 = 1;
    pub const POST_ID: i32 = 2;
}

pub struct Likes {
    insert: Stmt,
}

impl Likes {
    pub(super) fn prepare(conn: &Connection) -> Result<Self, Error> {
        Ok(Self {
            insert: conn.prepare(insert::SQL)?,
        })
    }

    pub fn add(&mut self, user: UserId, post: PostId) -> Result<Insert, Error> {
        let mut query = self.insert.query();
        query.bind_i64(insert::USER_ID, user.get())?;
        query.bind_i64(insert::POST_ID, post.get())?;

        if let Err(e) = query.step() {
            if e.code == sqlite::SQLITE_CONSTRAINT_FOREIGNKEY {
                return Ok(Insert::MissingParent);
            }
            return Err(e);
        }

        if query.changes() == 0 {
            return Ok(Insert::Duplicate);
        }
        Ok(Insert::Inserted)
    }
}
