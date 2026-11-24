use jsonwebtoken::{Algorithm, DecodingKey, Validation};
use serde::Deserialize;
use serde_json::Value;

use crate::app::UserId;

pub struct Claims {
    pub user_id: UserId,
    pub username: String,
}

#[derive(Deserialize)]
struct Payload {
    #[serde(default)]
    sub: Value,
    #[serde(default)]
    username: Value,
}

pub struct Auth {
    secret: DecodingKey,
    validation: Validation,
}

pub enum AuthError {
    NotBearer,
    InvalidToken,
    InvalidPayload,
}

impl Auth {
    pub fn new(secret: &[u8]) -> Result<Self, jsonwebtoken::errors::Error> {
        let mut validation = Validation::new(Algorithm::HS256);
        validation.leeway = 0;
        let s = Self {
            secret: DecodingKey::from_secret(secret)?,
            validation,
        };
        Ok(s)
    }

    pub fn validate(&self, authorization: &[u8]) -> Result<Claims, AuthError> {
        let token = authorization
            .strip_prefix(b"Bearer ")
            .ok_or(AuthError::NotBearer)?;

        let token = str::from_utf8(token).map_err(|_| AuthError::InvalidToken)?;
        let Ok(data) = jsonwebtoken::decode::<Payload>(token, &self.secret, &self.validation)
        else {
            return Err(AuthError::InvalidToken);
        };

        let Value::String(sub) = data.claims.sub else {
            return Err(AuthError::InvalidPayload);
        };
        let Value::String(username) = data.claims.username else {
            return Err(AuthError::InvalidPayload);
        };

        let user_id = UserId::parse(sub.as_str()).ok_or(AuthError::InvalidPayload)?;
        let c = Claims { user_id, username };
        Ok(c)
    }
}
