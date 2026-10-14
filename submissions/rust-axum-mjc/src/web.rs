use crate::{
    auth::Auth,
    db::{Db, DbError},
};
use axum::{
    body::{Body, Bytes, HttpBody},
    extract::{FromRequest, State},
    http::{
        HeaderMap, Method, Request, StatusCode,
        header::{ALLOW, CONTENT_LENGTH, CONTENT_TYPE, HeaderValue},
    },
    response::{IntoResponse, Response as AxumResponse},
};
use futures_util::FutureExt;
use http_body_util::{BodyExt, Full};
use hyper::{body::Incoming as HyperIncoming, service::Service as HyperService};
use serde::{Deserialize, Serialize};
use serde_json::value::RawValue;
use std::{borrow::Cow, convert::Infallible, sync::Arc, time::Instant};
use tower::{Service, service_fn};

pub type Response = AxumResponse<Full<Bytes>>;

struct AppState {
    db: Db,
    auth: Auth,
    started: Instant,
}

pub fn router<B>(
    db: Db,
    auth: Auth,
) -> impl Clone
+ Service<Request<B>, Response = Response, Error = Infallible, Future: Send + 'static>
+ Send
+ 'static
where
    B: HttpBody<Data = Bytes> + Send + 'static,
    B::Error: Into<axum::BoxError> + 'static,
{
    let state = app_state(db, auth);
    service_fn(move |request: Request<B>| {
        dispatch(Arc::clone(&state), request).map(Ok::<_, Infallible>)
    })
}

/// The production Hyper service. Unlike `router`, this avoids the Tower
/// adapter used by the test service path.
pub fn hyper_router(
    db: Db,
    auth: Auth,
) -> impl Clone
+ HyperService<
    Request<HyperIncoming>,
    Response = Response,
    Error = Infallible,
    Future: Send + 'static,
> + Send
+ 'static {
    let state = app_state(db, auth);
    hyper::service::service_fn(move |request: Request<HyperIncoming>| {
        dispatch(Arc::clone(&state), request).map(Ok::<_, Infallible>)
    })
}

fn app_state(db: Db, auth: Auth) -> Arc<AppState> {
    Arc::new(AppState {
        db,
        auth,
        started: Instant::now(),
    })
}

enum Route<'a> {
    Health,
    Feed,
    Create,
    Post(&'a str),
    Like(&'a str),
    Missing,
}

fn route(path: &str) -> Route<'_> {
    match path {
        "/health" => Route::Health,
        "/feed" => Route::Feed,
        "/posts" => Route::Create,
        _ => {
            let Some(rest) = path.strip_prefix("/posts/") else {
                return Route::Missing;
            };
            if let Some(id) = rest.strip_suffix("/like")
                && !id.is_empty()
                && !id.contains('/')
            {
                return Route::Like(id);
            }
            if !rest.is_empty() && !rest.contains('/') {
                return Route::Post(rest);
            }
            Route::Missing
        }
    }
}

async fn dispatch<B>(state: Arc<AppState>, request: Request<B>) -> Response
where
    B: HttpBody<Data = Bytes> + Send + 'static,
    B::Error: Into<axum::BoxError> + 'static,
{
    let (mut parts, body) = request.into_parts();
    let method = parts.method.clone();
    let matched = route(parts.uri.path());
    let mut headers = parts.headers;
    let response = match matched {
        Route::Health | Route::Feed | Route::Post(_)
            if method != Method::GET && method != Method::HEAD =>
        {
            method_not_allowed(&mut headers, "GET,HEAD")
        }
        Route::Create | Route::Like(_) if method != Method::POST => {
            method_not_allowed(&mut headers, "POST")
        }
        Route::Health => health(State(state), &mut headers).await,
        Route::Feed => feed(State(state), &mut headers).await,
        Route::Post(raw_id) => match crate::post_id::parse_post_id(raw_id) {
            Some(id) => get_post(State(state), id, &mut headers).await,
            None => error(&mut headers, StatusCode::BAD_REQUEST, "invalid post id"),
        },
        Route::Like(raw_id) => like_post(State(state), raw_id, &mut headers).await,
        Route::Create => {
            let mut request = Request::new(Body::new(body));
            *request.extensions_mut() = std::mem::take(&mut parts.extensions);
            match Bytes::from_request(request, &state).await {
                Ok(bytes) => create_post(State(state), &mut headers, bytes).await,
                Err(rejection) => {
                    let (parts, body) = rejection.into_response().into_parts();
                    let body = body
                        .collect()
                        .await
                        .expect("Axum rejection bodies are infallible")
                        .to_bytes()
                        .to_vec();
                    headers.clear();
                    headers.extend(parts.headers);
                    response(parts.status, body, &mut headers)
                }
            }
        }
        Route::Missing => error(&mut headers, StatusCode::NOT_FOUND, "not found"),
    };

    let mut response = response;
    if method == Method::HEAD
        && !response.headers().contains_key(CONTENT_LENGTH)
        && let Some(size) = response.body().size_hint().exact()
    {
        response
            .headers_mut()
            .insert(CONTENT_LENGTH, HeaderValue::from(size));
    }
    if method == Method::HEAD {
        *response.body_mut() = Full::new(Bytes::new());
    }
    response
}

fn method_not_allowed(headers: &mut HeaderMap, allow: &'static str) -> Response {
    headers.clear();
    headers.insert(ALLOW, allow.parse().expect("static Allow value"));
    response(StatusCode::METHOD_NOT_ALLOWED, Vec::new(), headers)
}

#[derive(Serialize)]
struct ErrorBody<'a> {
    error: &'a str,
}

fn error(headers: &mut HeaderMap, status: StatusCode, message: &str) -> Response {
    json_response(headers, status, ErrorBody { error: message })
}

fn encoded_json(headers: &mut HeaderMap, status: StatusCode, body: impl Into<Bytes>) -> Response {
    headers.clear();
    headers.insert(CONTENT_TYPE, HeaderValue::from_static("application/json"));
    response(status, body, headers)
}

fn json_response<T: Serialize>(headers: &mut HeaderMap, status: StatusCode, body: T) -> Response {
    let Ok(body) = serde_json::to_vec(&body) else {
        return plain_text_response(
            headers,
            StatusCode::INTERNAL_SERVER_ERROR,
            "Failed to serialize JSON body",
        );
    };
    encoded_json(headers, status, body)
}

fn plain_text_response(
    headers: &mut HeaderMap,
    status: StatusCode,
    body: &'static str,
) -> Response {
    headers.clear();
    headers.insert(
        CONTENT_TYPE,
        HeaderValue::from_static("text/plain; charset=utf-8"),
    );
    response(status, body.as_bytes().to_vec(), headers)
}

fn response(status: StatusCode, body: impl Into<Bytes>, headers: &mut HeaderMap) -> Response {
    let mut response = Response::new(Full::new(body.into()));
    *response.status_mut() = status;
    *response.headers_mut() = std::mem::take(headers);
    response
}

fn internal_error(headers: &mut HeaderMap, error_value: DbError) -> Response {
    if matches!(&error_value, DbError::WorkerBusy) {
        return error(headers, StatusCode::SERVICE_UNAVAILABLE, "database busy");
    }
    eprintln!("database request failed: {error_value}");
    error(
        headers,
        StatusCode::INTERNAL_SERVER_ERROR,
        "internal server error",
    )
}

#[derive(Serialize)]
struct HealthBody {
    status: &'static str,
    db: &'static str,
    uptime_s: u64,
}

#[derive(Serialize)]
struct DegradedHealth {
    status: &'static str,
    db: &'static str,
    error: String,
}

async fn health(State(state): State<Arc<AppState>>, headers: &mut HeaderMap) -> Response {
    match state.db.health().await {
        Ok(()) => json_response(
            headers,
            StatusCode::OK,
            HealthBody {
                status: "ok",
                db: "ok",
                uptime_s: state.started.elapsed().as_secs(),
            },
        ),
        Err(error_value) => json_response(
            headers,
            StatusCode::SERVICE_UNAVAILABLE,
            DegradedHealth {
                status: "degraded",
                db: "unreachable",
                error: error_value.to_string(),
            },
        ),
    }
}

async fn feed(State(state): State<Arc<AppState>>, headers: &mut HeaderMap) -> Response {
    match state.db.feed().await {
        Ok(body) => encoded_json(headers, StatusCode::OK, body),
        Err(error_value) => internal_error(headers, error_value),
    }
}

async fn get_post(
    State(state): State<Arc<AppState>>,
    id: i64,
    headers: &mut HeaderMap,
) -> Response {
    match state.db.post(id).await {
        Ok(Some(body)) => encoded_json(headers, StatusCode::OK, body),
        Ok(None) => error(headers, StatusCode::NOT_FOUND, "post not found"),
        Err(error_value) => internal_error(headers, error_value),
    }
}

#[derive(Deserialize)]
struct Incoming<'a> {
    // A direct Cow field lets serde borrow unescaped JSON strings. Missing body
    // defaults to an empty string, which follows the same required-body check.
    #[serde(default, borrow)]
    body: Cow<'a, str>,
}

fn post_body(bytes: &Bytes) -> Result<Bytes, &'static str> {
    let object_start = bytes.iter().position(|byte| !byte.is_ascii_whitespace());
    if !object_start.is_some_and(|index| bytes[index] == b'{') {
        let _: &RawValue = serde_json::from_slice(bytes).map_err(|_| "malformed JSON body")?;
        return Err("body is required");
    }
    // Retain borrowed strings and parse accepted requests once. Rejected
    // type errors get a full-document validation pass to preserve error order.
    let incoming: Incoming<'_> = match serde_json::from_slice(bytes) {
        Ok(incoming) => incoming,
        Err(error) if error.is_data() => {
            // Deserialization can stop at a type error before reaching invalid
            // trailing JSON. Validate those uncommon rejected requests fully
            // to preserve malformed-JSON precedence.
            let _: &RawValue = serde_json::from_slice(bytes).map_err(|_| "malformed JSON body")?;
            return Err("body is required");
        }
        Err(_) => return Err("malformed JSON body"),
    };
    let trimmed = incoming.body.trim();
    if trimmed.is_empty() {
        return Err("body is required");
    }
    if trimmed.chars().count() > 500 {
        return Err("body must be at most 500 characters");
    }
    let start = incoming.body.len() - incoming.body.trim_start().len();
    let end = start + trimmed.len();
    Ok(match incoming.body {
        Cow::Borrowed(text) => bytes.slice_ref(text.trim().as_bytes()),
        // Escapes require decoding; keep that allocation and slice its padding.
        Cow::Owned(text) => Bytes::from(text).slice(start..end),
    })
}

async fn create_post(
    State(state): State<Arc<AppState>>,
    headers: &mut HeaderMap,
    bytes: Bytes,
) -> Response {
    let user = match state.auth.authenticate(headers) {
        Ok(user) => user,
        Err(message) => return error(headers, StatusCode::UNAUTHORIZED, message),
    };
    let body = match post_body(&bytes) {
        Ok(body) => body,
        Err(message) => return error(headers, StatusCode::BAD_REQUEST, message),
    };
    match state
        .db
        .create_post(user.user_id, body, user.username)
        .await
    {
        Ok(body) => encoded_json(headers, StatusCode::CREATED, body),
        Err(error_value) => internal_error(headers, error_value),
    }
}

#[derive(Serialize)]
struct LikeBody {
    liked: bool,
    already_liked: bool,
    post_id: i64,
}

async fn like_post(
    State(state): State<Arc<AppState>>,
    raw_id: &str,
    headers: &mut HeaderMap,
) -> Response {
    let user_id = match state.auth.authenticate_id(headers) {
        Ok(user_id) => user_id,
        Err(message) => return error(headers, StatusCode::UNAUTHORIZED, message),
    };
    let Some(post_id) = crate::post_id::parse_post_id(raw_id) else {
        return error(headers, StatusCode::BAD_REQUEST, "invalid post id");
    };
    match state.db.like(user_id, post_id).await {
        Ok(Some(already_liked)) => json_response(
            headers,
            if already_liked {
                StatusCode::OK
            } else {
                StatusCode::CREATED
            },
            LikeBody {
                liked: true,
                already_liked,
                post_id,
            },
        ),
        Ok(None) => error(headers, StatusCode::NOT_FOUND, "post not found"),
        Err(error_value) => internal_error(headers, error_value),
    }
}

#[cfg(test)]
include!("tests/web.rs");
