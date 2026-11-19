use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};

use axum::{
    async_trait,
    extract::FromRequestParts,
    http::{StatusCode, header, request::Parts},
    response::Response,
};
use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use hmac::{Hmac, Mac};
use serde_json::Value;
use sha2::Sha256;

use crate::{
    AppState,
    handlers::{json_err, parse_positive_id},
};

type HmacSha256 = Hmac<Sha256>;

pub struct AuthUser {
    pub user_id: i64,
    pub username: String,
}

#[async_trait]
impl FromRequestParts<Arc<AppState>> for AuthUser {
    type Rejection = Response;

    async fn from_request_parts(
        parts: &mut Parts,
        state: &Arc<AppState>,
    ) -> Result<Self, Self::Rejection> {
        let auth = parts
            .headers
            .get(header::AUTHORIZATION)
            .and_then(|v| v.to_str().ok())
            .unwrap_or("");

        let Some(token) = auth.strip_prefix("Bearer ") else {
            return Err(json_err(StatusCode::UNAUTHORIZED, "missing bearer token"));
        };

        let mut segs = token.split('.');
        let (Some(h_b64), Some(p_b64), Some(s_b64), None) =
            (segs.next(), segs.next(), segs.next(), segs.next())
        else {
            return Err(json_err(
                StatusCode::UNAUTHORIZED,
                "invalid or expired token",
            ));
        };

        let header_bytes = URL_SAFE_NO_PAD
            .decode(h_b64)
            .map_err(|_| json_err(StatusCode::UNAUTHORIZED, "invalid or expired token"))?;
        let header_val: Value = serde_json::from_slice(&header_bytes)
            .map_err(|_| json_err(StatusCode::UNAUTHORIZED, "invalid or expired token"))?;
        if header_val.get("alg").and_then(Value::as_str) != Some("HS256") {
            return Err(json_err(
                StatusCode::UNAUTHORIZED,
                "invalid or expired token",
            ));
        }

        let sig_bytes = URL_SAFE_NO_PAD
            .decode(s_b64)
            .map_err(|_| json_err(StatusCode::UNAUTHORIZED, "invalid or expired token"))?;
        let mut mac = HmacSha256::new_from_slice(&state.jwt_secret)
            .map_err(|_| json_err(StatusCode::UNAUTHORIZED, "invalid or expired token"))?;
        mac.update(h_b64.as_bytes());
        mac.update(b".");
        mac.update(p_b64.as_bytes());
        mac.verify_slice(&sig_bytes)
            .map_err(|_| json_err(StatusCode::UNAUTHORIZED, "invalid or expired token"))?;

        let payload_bytes = URL_SAFE_NO_PAD
            .decode(p_b64)
            .map_err(|_| json_err(StatusCode::UNAUTHORIZED, "invalid or expired token"))?;
        let payload: Value = serde_json::from_slice(&payload_bytes)
            .map_err(|_| json_err(StatusCode::UNAUTHORIZED, "invalid or expired token"))?;

        let exp = payload
            .get("exp")
            .and_then(Value::as_f64)
            .ok_or_else(|| json_err(StatusCode::UNAUTHORIZED, "invalid or expired token"))?;
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs_f64();
        if now >= exp {
            return Err(json_err(
                StatusCode::UNAUTHORIZED,
                "invalid or expired token",
            ));
        }

        // The spec requires `sub` to be a positive integer *string*.
        let user_id = payload
            .get("sub")
            .and_then(Value::as_str)
            .and_then(parse_positive_id)
            .ok_or_else(|| json_err(StatusCode::UNAUTHORIZED, "invalid token payload"))?;

        let username = payload
            .get("username")
            .and_then(Value::as_str)
            .ok_or_else(|| json_err(StatusCode::UNAUTHORIZED, "invalid token payload"))?
            .to_owned();

        Ok(AuthUser { user_id, username })
    }
}
