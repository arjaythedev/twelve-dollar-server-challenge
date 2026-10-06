use axum::http::{HeaderMap, StatusCode, header};
use axum::response::Response;

use crate::model::Claims;
use crate::response::error;
use crate::state::AppState;
use crate::util::parse_id;

pub fn authenticate(state: &AppState, headers: &HeaderMap) -> Result<(i64, String), Response> {
    let token = match headers.get(header::AUTHORIZATION).and_then(|v| v.to_str().ok()) {
        Some(h) => match h.strip_prefix("Bearer ") {
            Some(t) => t,
            None => return Err(error(StatusCode::UNAUTHORIZED, "missing bearer token")),
        },
        None => return Err(error(StatusCode::UNAUTHORIZED, "missing bearer token")),
    };

    let data = jsonwebtoken::decode::<Claims>(token, &state.jwt, &state.validation)
        .map_err(|_| error(StatusCode::UNAUTHORIZED, "invalid or expired token"))?;

    let user_id = data.claims.sub.as_deref().and_then(parse_id);
    match (user_id, data.claims.username) {
        (Some(id), Some(username)) => Ok((id, username)),
        _ => Err(error(StatusCode::UNAUTHORIZED, "invalid token payload")),
    }
}
