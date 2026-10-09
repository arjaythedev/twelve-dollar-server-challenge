use axum::http::{HeaderMap, header::AUTHORIZATION};
use jsonwebtoken::{Algorithm, DecodingKey, Validation, decode};
use serde_json::Value;
use std::time::{SystemTime, UNIX_EPOCH};

#[derive(Clone)]
pub struct Auth {
    key: DecodingKey,
    validation: Validation,
}

#[derive(Debug, PartialEq, Eq)]
pub struct User {
    pub user_id: i64,
    pub username: String,
}

impl Auth {
    pub fn new(secret: impl AsRef<[u8]>) -> Self {
        let mut validation = Validation::new(Algorithm::HS256);
        // Check expiration ourselves so exp == now is expired and claim type errors
        // have the same response as expired tokens. No clock-skew allowance.
        validation.validate_exp = false;
        validation.validate_aud = false;
        validation.required_spec_claims.clear();
        Self {
            key: DecodingKey::from_secret(secret.as_ref()),
            validation,
        }
    }

    pub fn authenticate(&self, headers: &HeaderMap) -> Result<User, &'static str> {
        let token = headers
            .get(AUTHORIZATION)
            .and_then(|header| header.as_bytes().strip_prefix(b"Bearer "))
            .ok_or("missing bearer token")?;
        let mut claims = decode::<Value>(token, &self.key, &self.validation)
            .map_err(|_| "invalid or expired token")?
            .claims;
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|_| "invalid or expired token")?
            .as_secs();
        let expires = claims
            .get("exp")
            .and_then(Value::as_u64)
            .ok_or("invalid or expired token")?;
        if expires <= now {
            return Err("invalid or expired token");
        }
        let user_id = claims
            .get("sub")
            .and_then(Value::as_str)
            .and_then(positive_id)
            .ok_or("invalid token payload")?;
        let username = match claims.get_mut("username").map(Value::take) {
            Some(Value::String(username)) => username,
            _ => return Err("invalid token payload"),
        };
        Ok(User { user_id, username })
    }
}

pub(crate) fn positive_id(value: &str) -> Option<i64> {
    if value.is_empty() || !value.bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    value.parse::<i64>().ok().filter(|id| *id > 0)
}

#[cfg(test)]
include!("tests/auth.rs");
