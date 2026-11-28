use crate::http::Method;

pub enum Route<'a> {
    Health,
    Feed,
    CreatePost,
    GetPost(&'a [u8]),
    LikePost(&'a [u8]),
    NotFound,
}

impl<'a> Route<'a> {
    pub fn parse(method: Method, path: &'a [u8]) -> Self {
        match (method, path) {
            (Method::Get, b"/health") => return Self::Health,
            (Method::Get, b"/feed") => return Self::Feed,
            (Method::Post, b"/posts") => return Self::CreatePost,
            _ => {}
        }

        let Some(rest) = path.strip_prefix(b"/posts/") else {
            return Self::NotFound;
        };
        match (method, rest.strip_suffix(b"/like")) {
            (Method::Post, Some(id)) => Self::LikePost(id),
            (Method::Get, None) if !rest.contains(&b'/') => Self::GetPost(rest),
            _ => Self::NotFound,
        }
    }
}
