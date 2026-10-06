use std::sync::Arc;

use axum::extract::State;
use axum::http::StatusCode;
use axum::response::Response;

use crate::db::with_read;
use crate::model::{HealthErr, HealthOk};
use crate::response::json;
use crate::state::AppState;

pub async fn health(State(state): State<Arc<AppState>>) -> Response {
    let probe = with_read(|conn| conn.query_row("SELECT 1", [], |_| Ok(())));
    match probe {
        Ok(()) => json(
            StatusCode::OK,
            HealthOk {
                status: "ok",
                db: "ok",
                uptime_s: state.start.elapsed().as_secs(),
            },
        ),
        Err(e) => json(
            StatusCode::SERVICE_UNAVAILABLE,
            HealthErr {
                status: "degraded",
                db: "unreachable",
                error: e.to_string(),
            },
        ),
    }
}
