use crate::{
    app::{PostId, UserId},
    db::{
        conn::Connection,
        error::Error,
        queries::Next,
        stmt::{Query, Step, Stmt},
    },
};

// Column layout shared by `by_id` and `feed`.
mod post {
    pub const ID: i32 = 0;
    pub const BODY: i32 = 1;
    pub const CREATED_AT: i32 = 2;
    pub const AUTHOR: i32 = 3;
    pub const LIKE_COUNT: i32 = 4;
}

mod by_id {
    pub const SQL: &str = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1";
    pub const POST_ID: i32 = 1;
}

mod feed {
    pub const SQL: &str = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
}

mod insert {
    pub const SQL: &str =
        "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at";
    pub const USER_ID: i32 = 1;
    pub const BODY: i32 = 2;

    pub const OUT_ID: i32 = 0;
    pub const OUT_CREATED_AT: i32 = 1;
}

/// A post row, borrowed from the statement until the next step.
pub struct Post<'r> {
    pub id: i64,
    pub body: &'r [u8],
    pub created_at: &'r [u8],
    pub author: &'r [u8],
    pub like_count: i64,
}

/// The columns sqlite generates for a freshly inserted post.
pub struct Created<'r> {
    pub id: i64,
    pub created_at: &'r [u8],
}

/// Guard over a post query; the statement resets when it drops.
pub struct PostRows<'q> {
    query: Query<'q>,
}

impl PostRows<'_> {
    pub fn next(&mut self) -> Result<Next<Post<'_>>, Error> {
        let Step::Row(row) = self.query.step()? else {
            return Ok(Next::Done);
        };
        Ok(Next::Row(Post {
            id: row.i64(post::ID),
            body: row.text(post::BODY),
            created_at: row.text(post::CREATED_AT),
            author: row.text(post::AUTHOR),
            like_count: row.i64(post::LIKE_COUNT),
        }))
    }
}

/// Guard over an insert; the row is written on the first `next`.
pub struct CreatedRows<'q> {
    query: Query<'q>,
}

impl CreatedRows<'_> {
    pub fn next(&mut self) -> Result<Next<Created<'_>>, Error> {
        let Step::Row(row) = self.query.step()? else {
            return Ok(Next::Done);
        };
        Ok(Next::Row(Created {
            id: row.i64(insert::OUT_ID),
            created_at: row.text(insert::OUT_CREATED_AT),
        }))
    }
}

pub struct Posts {
    by_id: Stmt,
    feed: Stmt,
    insert: Stmt,
}

impl Posts {
    pub(super) fn prepare(conn: &Connection) -> Result<Self, Error> {
        Ok(Self {
            by_id: conn.prepare(by_id::SQL)?,
            feed: conn.prepare(feed::SQL)?,
            insert: conn.prepare(insert::SQL)?,
        })
    }

    /// Yields the post, or `Done` if it doesn't exist.
    pub fn get(&mut self, id: PostId) -> Result<PostRows<'_>, Error> {
        let mut query = self.by_id.query();
        query.bind_i64(by_id::POST_ID, id.get())?;
        Ok(PostRows { query })
    }

    /// Yields the 20 newest posts, newest first.
    pub fn feed(&mut self) -> PostRows<'_> {
        PostRows {
            query: self.feed.query(),
        }
    }

    pub fn insert<'q>(
        &'q mut self,
        user: UserId,
        body: &'q [u8],
    ) -> Result<CreatedRows<'q>, Error> {
        let mut query = self.insert.query();
        query.bind_i64(insert::USER_ID, user.get())?;
        query.bind_text(insert::BODY, body)?;
        Ok(CreatedRows { query })
    }
}
