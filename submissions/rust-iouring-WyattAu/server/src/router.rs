//! Route dispatch with SPEC-exact error precedence.
//!
//! Precedence (challenge/SPEC.md):
//! - authenticated routes: auth (401 rows) THEN id parse (400) THEN existence (404)
//! - unknown path/method: 404 `not found`
//! - every error body is exactly `{"error":"<message>"}`

use crate::jwt::JwtError;
use uring_http::http::{Method, Request};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Route {
    Health,
    Feed,
    PostGet,
    PostCreate,
    PostLike,
}

/// Full dispatch outcome: route + the already-validated pieces, or the
/// SPEC error row. Total over arbitrary inputs.
#[derive(Debug, PartialEq, Eq)]
pub enum Dispatch<'a> {
    Route(Route, &'a str /* token sub, when authed */),
    /// (status, message) — bytes match the SPEC table exactly.
    Error((u16, &'static str)),
}

/// Status/message pairs from the SPEC errors table.
pub mod errors {
    pub const NOT_FOUND: (u16, &str) = (404, "not found");
    pub const INVALID_POST_ID: (u16, &str) = (400, "invalid post id");
    pub const POST_NOT_FOUND: (u16, &str) = (404, "post not found");
    pub const MISSING_BEARER: (u16, &str) = (401, "missing bearer token");
    pub const INVALID_TOKEN: (u16, &str) = (401, "invalid or expired token");
    pub const INVALID_PAYLOAD: (u16, &str) = (401, "invalid token payload");
    pub const MALFORMED_JSON: (u16, &str) = (400, "malformed JSON body");
    pub const BODY_REQUIRED: (u16, &str) = (400, "body is required");
    pub const BODY_TOO_LONG: (u16, &str) = (400, "body must be at most 500 characters");
    pub const INTERNAL: (u16, &str) = (500, "internal server error");
}

/// SPEC error row for a JWT failure (used by the pass layer).
pub fn jwt_row(e: JwtError) -> (u16, &'static str) {
    jwt_err_row(e)
}

fn jwt_err_row(e: JwtError) -> (u16, &'static str) {
    match e {
        JwtError::MissingBearer => errors::MISSING_BEARER,
        JwtError::InvalidToken => errors::INVALID_TOKEN,
        JwtError::InvalidPayload => errors::INVALID_PAYLOAD,
    }
}

/// `:id` must be a positive integer (SPEC: 0, -1, abc, 1.5 are invalid).
/// All-digits, parse, > 0. Overflowing u64 is "not a positive integer".
fn parse_post_id(seg: &[u8]) -> Option<i64> {
    if seg.is_empty() || seg.len() > 18 {
        return None;
    }
    let mut v: i64 = 0;
    for &b in seg {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((b - b'0') as i64)?;
    }
    (v > 0).then_some(v)
}

/// Map (method, target) to a route or error. `target` excludes the query
/// string (the challenge API has none).
pub fn dispatch<'a>(req: &Request<'a>) -> Dispatch<'a> {
    match req.method {
        Method::Get | Method::Other => {
            // GET routes; unknown methods on known paths are 404 per SPEC
            if req.method == Method::Other {
                return Dispatch::Error(errors::NOT_FOUND);
            }
            match req.target {
                b"/health" => Dispatch::Route(Route::Health, ""),
                b"/feed" => Dispatch::Route(Route::Feed, ""),
                t if t.starts_with(b"/posts/") => {
                    let seg = &t[b"/posts/".len()..];
                    if seg.contains(&b'/') {
                        // a deeper path shape is a different (unknown) path
                        return Dispatch::Error(errors::NOT_FOUND);
                    }
                    match parse_post_id(seg) {
                        Some(_) => Dispatch::Route(Route::PostGet, ""),
                        None => Dispatch::Error(errors::INVALID_POST_ID),
                    }
                }
                _ => Dispatch::Error(errors::NOT_FOUND),
            }
        }
        Method::Post => {
            if req.target == b"/posts" {
                return Dispatch::Route(Route::PostCreate, "");
            }
            if let Some(rest) = req.target.strip_prefix(b"/posts/") {
                // like route: "/posts/:id/like" — id parse precedes existence,
                // and auth precedes id (handled by the handler via Dispatch
                // carrying the auth decision point).
                if rest.ends_with(b"/like") {
                    // auth precedes id validation (SPEC); the pass layer
                    // parses the id after the 401 checks
                    return Dispatch::Route(Route::PostLike, "");
                }
                return Dispatch::Error(errors::NOT_FOUND);
            }
            Dispatch::Error(errors::NOT_FOUND)
        }
    }
}

/// Auth check for authenticated routes. SPEC: check auth FIRST, then the id.
/// The router defers id-parse errors so the handler can return 401 first —
/// `dispatch` returns the id error only for the caller to order correctly.
pub fn auth_error(
    auth: Option<&[u8]>,
    mac: &crate::jwt::KeyedMac,
    now: i64,
) -> Option<(u16, &'static str)> {
    crate::jwt::verify(auth, mac, now).err().map(jwt_err_row)
}

#[cfg(test)]
mod tests {
    use super::*;
    use uring_http::http::parse;

    fn route(req: &str) -> Dispatch<'_> {
        dispatch(&parse(req.as_bytes()).unwrap())
    }

    #[test]
    fn known_routes() {
        assert!(matches!(route("GET /health HTTP/1.1\r\n\r\n"), Dispatch::Route(Route::Health, _)));
        assert!(matches!(route("GET /feed HTTP/1.1\r\n\r\n"), Dispatch::Route(Route::Feed, _)));
        assert!(matches!(route("GET /posts/42 HTTP/1.1\r\n\r\n"), Dispatch::Route(Route::PostGet, _)));
        assert!(matches!(route("POST /posts HTTP/1.1\r\n\r\n"), Dispatch::Route(Route::PostCreate, _)));
        assert!(matches!(route("POST /posts/42/like HTTP/1.1\r\n\r\n"), Dispatch::Route(Route::PostLike, _)));
    }

    #[test]
    fn invalid_ids() {
        for bad in ["0", "-1", "abc", "1.5", "12x", "99999999999999999999"] {
            let req = format!("GET /posts/{bad} HTTP/1.1\r\n\r\n");
            let r = route(&req);
            assert_eq!(r, Dispatch::Error(errors::INVALID_POST_ID), "{bad}");
        }
    }

    #[test]
    fn unknown_paths_and_methods() {
        assert_eq!(route("GET /metrics HTTP/1.1\r\n\r\n"), Dispatch::Error(errors::NOT_FOUND));
        assert_eq!(route("PUT /posts HTTP/1.1\r\n\r\n"), Dispatch::Error(errors::NOT_FOUND));
        assert_eq!(route("POST /posts/42 HTTP/1.1\r\n\r\n"), Dispatch::Error(errors::NOT_FOUND));
        assert_eq!(route("GET /posts/42/like HTTP/1.1\r\n\r\n"), Dispatch::Error(errors::NOT_FOUND));
    }
}
