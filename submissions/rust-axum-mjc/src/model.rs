use serde::Serialize;

#[derive(Debug, Serialize)]
pub struct Post<'a> {
    pub id: i64,
    pub body: &'a str,
    pub created_at: &'a str,
    pub author: &'a str,
    pub like_count: i64,
}
