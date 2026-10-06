use axum::Json;
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use serde::Serialize;

use crate::model::ErrorResponse;

pub fn json<T: Serialize>(status: StatusCode, body: T) -> Response {
    (status, Json(body)).into_response()
}

pub fn error(status: StatusCode, message: &'static str) -> Response {
    json(status, ErrorResponse { error: message })
}
