use rusqlite::Row;
use serde::{Deserialize, Serialize};

#[derive(Serialize)]
pub struct Post {
    pub id: i64,
    pub body: String,
    pub created_at: String,
    pub author: String,
    pub like_count: i64,
}

impl Post {
    pub fn from_row(row: &Row) -> rusqlite::Result<Post> {
        Ok(Post {
            id: row.get(0)?,
            body: row.get(1)?,
            created_at: row.get(2)?,
            author: row.get(3)?,
            like_count: row.get(4)?,
        })
    }
}

#[derive(Serialize)]
pub struct FeedResponse {
    pub posts: Vec<Post>,
}

#[derive(Serialize)]
pub struct PostResponse {
    pub post: Post,
}

#[derive(Serialize)]
pub struct LikeResponse {
    pub liked: bool,
    pub already_liked: bool,
    pub post_id: i64,
}

#[derive(Serialize)]
pub struct HealthOk {
    pub status: &'static str,
    pub db: &'static str,
    pub uptime_s: u64,
}

#[derive(Serialize)]
pub struct HealthErr {
    pub status: &'static str,
    pub db: &'static str,
    pub error: String,
}

#[derive(Serialize)]
pub struct ErrorResponse {
    pub error: &'static str,
}

#[derive(Deserialize)]
pub struct Claims {
    pub sub: Option<String>,
    pub username: Option<String>,
}
