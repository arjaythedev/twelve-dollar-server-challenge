use std::borrow::Cow;
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
use serde::Deserialize;
use serde_json::Value;
use sha2::Sha256;

use crate::{
    AppState,
    handlers::{json_err, parse_positive_id},
};

pub type HmacSha256 = Hmac<Sha256>;

// Seed tokens are a few hundred bytes; larger tokens fall back to the heap.
const HEADER_SCRATCH: usize = 256;
const PAYLOAD_SCRATCH: usize = 768;
const SIGNATURE_SCRATCH: usize = 64;

pub struct AuthUser {
    pub user_id: i64,
    pub username: String,
}

#[derive(Deserialize)]
struct JwtHeader<'a> {
    alg: Option<&'a str>,
}

#[derive(Deserialize)]
struct Claims {
    exp: Option<f64>,
    sub: Option<Value>,
    username: Option<Value>,
}

fn invalid_token() -> Response {
    json_err(StatusCode::UNAUTHORIZED, "invalid or expired token")
}

fn invalid_payload() -> Response {
    json_err(StatusCode::UNAUTHORIZED, "invalid token payload")
}

/// Decodes into the caller's stack buffer, allocating only when the segment
/// is larger than that buffer.
fn decode_segment<'a>(input: &str, scratch: &'a mut [u8]) -> Option<Cow<'a, [u8]>> {
    match URL_SAFE_NO_PAD.decode_slice(input.as_bytes(), scratch) {
        Ok(len) => Some(Cow::Borrowed(&scratch[..len])),
        Err(_) => URL_SAFE_NO_PAD
            .decode(input.as_bytes())
            .ok()
            .map(Cow::Owned),
    }
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
            .and_then(|value| value.to_str().ok())
            .unwrap_or("");

        let Some(token) = auth.strip_prefix("Bearer ") else {
            return Err(json_err(StatusCode::UNAUTHORIZED, "missing bearer token"));
        };

        let mut segments = token.split('.');
        let (Some(header_b64), Some(payload_b64), Some(signature_b64), None) = (
            segments.next(),
            segments.next(),
            segments.next(),
            segments.next(),
        ) else {
            return Err(invalid_token());
        };

        let mut header_buf = [0u8; HEADER_SCRATCH];
        let header_bytes = decode_segment(header_b64, &mut header_buf).ok_or_else(invalid_token)?;
        let header: JwtHeader<'_> =
            serde_json::from_slice(header_bytes.as_ref()).map_err(|_| invalid_token())?;
        if header.alg != Some("HS256") {
            return Err(invalid_token());
        }

        // The signature covers the original base64url segments, so verify
        // before decoding or trusting the payload.
        let mut signature_buf = [0u8; SIGNATURE_SCRATCH];
        let signature =
            decode_segment(signature_b64, &mut signature_buf).ok_or_else(invalid_token)?;

        let mut mac = state.mac.clone();
        mac.update(header_b64.as_bytes());
        mac.update(b".");
        mac.update(payload_b64.as_bytes());
        mac.verify_slice(signature.as_ref())
            .map_err(|_| invalid_token())?;

        let mut payload_buf = [0u8; PAYLOAD_SCRATCH];
        let payload_bytes =
            decode_segment(payload_b64, &mut payload_buf).ok_or_else(invalid_token)?;
        let payload: Claims =
            serde_json::from_slice(payload_bytes.as_ref()).map_err(|_| invalid_token())?;

        let exp = payload.exp.ok_or_else(invalid_token)?;
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs_f64();
        if now >= exp {
            return Err(invalid_token());
        }

        // The spec requires `sub` to be a positive integer string.
        let user_id = match payload.sub {
            Some(Value::String(value)) => parse_positive_id(&value),
            _ => None,
        }
        .ok_or_else(invalid_payload)?;

        let username = match payload.username {
            Some(Value::String(value)) => value,
            _ => return Err(invalid_payload()),
        };

        Ok(AuthUser { user_id, username })
    }
}
