use std::time::Instant;

use jsonwebtoken::DecodingKey;
use jsonwebtoken::Validation;
use tokio::sync::mpsc;

use crate::db::Write;

pub struct AppState {
    pub writer: mpsc::Sender<Write>,
    pub jwt: DecodingKey,
    pub validation: Validation,
    pub start: Instant,
}
