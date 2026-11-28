use serde::Serialize;
use serde_json::Value;

use crate::{
    app::{Ctx, PostId, auth::Claims, error::ApiError, response::Response},
    db::{Insert, Next, Post},
};

const MAX_BODY_CHARS: usize = 500;

#[derive(Serialize)]
struct Healthy {
    status: &'static str,
    db: &'static str,
    uptime_s: u64,
}

#[derive(Serialize)]
struct Degraded {
    status: &'static str,
    db: &'static str,
    error: String,
}

#[derive(Serialize)]
struct PostJson<'a> {
    id: i64,
    body: &'a str,
    created_at: &'a str,
    author: &'a str,
    like_count: i64,
}

impl<'a> PostJson<'a> {
    fn from_row(p: &Post<'a>) -> Result<Self, ApiError> {
        Ok(Self {
            id: p.id,
            body: text(p.body)?,
            created_at: text(p.created_at)?,
            author: text(p.author)?,
            like_count: p.like_count,
        })
    }
}

#[derive(Serialize)]
struct OnePost<'a> {
    post: PostJson<'a>,
}

#[derive(Serialize)]
struct Liked {
    liked: bool,
    already_liked: bool,
    post_id: i64,
}

pub(super) fn health(ctx: &mut Ctx) -> Result<(), ApiError> {
    match ctx.state.db.health().ping() {
        Ok(()) => {
            let body = Healthy {
                status: "ok",
                db: "ok",
                uptime_s: ctx.state.started.elapsed().as_secs(),
            };
            write_json(ctx.res, &body);
        }
        Err(e) => {
            ctx.res.status = 503;
            let body = Degraded {
                status: "degraded",
                db: "unreachable",
                error: e.to_string(),
            };
            write_json(ctx.res, &body);
        }
    }
    Ok(())
}

pub(super) fn feed(ctx: &mut Ctx) -> Result<(), ApiError> {
    let mut rows = ctx.state.db.posts().feed();
    let out = &mut ctx.res.body;

    out.extend_from_slice(b"{\"posts\":[");
    let mut first = true;
    while let Next::Row(p) = rows.next()? {
        if !first {
            out.push(b',');
        }
        first = false;
        let _ = serde_json::to_writer(&mut *out, &PostJson::from_row(&p)?);
    }
    out.extend_from_slice(b"]}");
    Ok(())
}

pub(super) fn get_post(ctx: &mut Ctx, raw_id: &[u8]) -> Result<(), ApiError> {
    let id = parse_post_id(raw_id)?;
    let mut rows = ctx.state.db.posts().get(id)?;
    let Next::Row(p) = rows.next()? else {
        return Err(ApiError::POST_NOT_FOUND);
    };
    let body = OnePost {
        post: PostJson::from_row(&p)?,
    };
    write_json(ctx.res, &body);
    Ok(())
}

pub(super) fn create_post(ctx: &mut Ctx) -> Result<(), ApiError> {
    let claims = authenticate(ctx)?;

    let json: Value = serde_json::from_slice(ctx.req.body).map_err(|_| ApiError::MALFORMED_JSON)?;
    let Some(Value::String(body)) = json.get("body") else {
        return Err(ApiError::BODY_REQUIRED);
    };
    let body = body.trim();
    if body.is_empty() {
        return Err(ApiError::BODY_REQUIRED);
    }
    if body.chars().count() > MAX_BODY_CHARS {
        return Err(ApiError::BODY_TOO_LONG);
    }

    let mut rows = ctx
        .state
        .db
        .posts()
        .insert(claims.user_id, body.as_bytes())?;
    let Next::Row(created) = rows.next()? else {
        return Err(ApiError::INTERNAL);
    };

    ctx.res.status = 201;
    let res = OnePost {
        post: PostJson {
            id: created.id,
            body,
            created_at: text(created.created_at)?,
            author: &claims.username,
            like_count: 0,
        },
    };
    write_json(ctx.res, &res);
    Ok(())
}

pub(super) fn like_post(ctx: &mut Ctx, raw_id: &[u8]) -> Result<(), ApiError> {
    // auth is checked before the id
    let claims = authenticate(ctx)?;
    let post_id = parse_post_id(raw_id)?;

    let (status, already_liked) = match ctx.state.db.likes().add(claims.user_id, post_id)? {
        Insert::Inserted => (201, false),
        Insert::Duplicate => (200, true),
        Insert::MissingParent => return Err(ApiError::POST_NOT_FOUND),
    };

    ctx.res.status = status;
    let body = Liked {
        liked: true,
        already_liked,
        post_id: post_id.get(),
    };
    write_json(ctx.res, &body);
    Ok(())
}

fn authenticate(ctx: &Ctx) -> Result<Claims, ApiError> {
    let header = ctx.req.authorization.ok_or(ApiError::MISSING_TOKEN)?;
    let c = ctx.state.auth.validate(header)?;
    Ok(c)
}

fn parse_post_id(raw: &[u8]) -> Result<PostId, ApiError> {
    str::from_utf8(raw)
        .ok()
        .and_then(PostId::parse)
        .ok_or(ApiError::INVALID_POST_ID)
}

fn text(b: &[u8]) -> Result<&str, ApiError> {
    str::from_utf8(b).map_err(|_| ApiError::INTERNAL)
}

fn write_json<T: Serialize>(res: &mut Response, value: &T) {
    let _ = serde_json::to_writer(&mut res.body, value);
}
