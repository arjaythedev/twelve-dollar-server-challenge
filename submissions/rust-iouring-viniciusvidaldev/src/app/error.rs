use serde::Serialize;

use super::auth::AuthError;
use crate::db;

pub struct ApiError {
    pub status: u16,
    pub message: &'static str,
}

impl ApiError {
    pub const NOT_FOUND: Self = Self::new(404, "not found");
    pub const INVALID_POST_ID: Self = Self::new(400, "invalid post id");
    pub const POST_NOT_FOUND: Self = Self::new(404, "post not found");
    pub const MISSING_TOKEN: Self = Self::new(401, "missing bearer token");
    pub const INVALID_TOKEN: Self = Self::new(401, "invalid or expired token");
    pub const INVALID_TOKEN_PAYLOAD: Self = Self::new(401, "invalid token payload");
    pub const MALFORMED_JSON: Self = Self::new(400, "malformed JSON body");
    pub const BODY_REQUIRED: Self = Self::new(400, "body is required");
    pub const BODY_TOO_LONG: Self = Self::new(400, "body must be at most 500 characters");
    pub const INTERNAL: Self = Self::new(500, "internal server error");

    const fn new(status: u16, message: &'static str) -> Self {
        Self { status, message }
    }
}

#[derive(Serialize)]
pub struct ErrorBody {
    pub error: &'static str,
}

impl From<db::Error> for ApiError {
    fn from(e: db::Error) -> Self {
        eprintln!("{e}");
        Self::INTERNAL
    }
}

impl From<AuthError> for ApiError {
    fn from(e: AuthError) -> Self {
        match e {
            AuthError::NotBearer => Self::MISSING_TOKEN,
            AuthError::InvalidToken => Self::INVALID_TOKEN,
            AuthError::InvalidPayload => Self::INVALID_TOKEN_PAYLOAD,
        }
    }
}
