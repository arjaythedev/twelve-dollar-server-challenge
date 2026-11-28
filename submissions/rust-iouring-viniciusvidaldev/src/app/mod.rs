mod auth;
mod error;
mod handlers;
mod ids;
mod response;
mod route;

use std::time::Instant;

pub use auth::Auth;
pub use ids::{PostId, UserId};

use crate::{
    app::{
        error::{ApiError, ErrorBody},
        response::Response,
        route::Route,
    },
    db::Db,
    http::{self, Parse, Request},
};

pub enum Handled {
    NeedMore,
    Responded { consumed: usize },
    Close,
}

pub struct State {
    pub db: Db,
    pub auth: Auth,
    pub started: Instant,
}

pub struct Ctx<'a> {
    pub state: &'a mut State,
    pub req: &'a Request<'a>,
    pub res: &'a mut Response,
}

pub struct App {
    state: State,
    res: Response,
}

impl App {
    pub fn new(db: Db, auth: Auth) -> Self {
        let state = State {
            db,
            auth,
            started: Instant::now(),
        };
        Self {
            state,
            res: Response::new(),
        }
    }

    pub fn handle(&mut self, input: &[u8], out: &mut Vec<u8>) -> Handled {
        let req = match http::parse(&input) {
            Parse::Invalid => return Handled::Close,
            Parse::Partial => return Handled::NeedMore,
            Parse::Done(req) => req,
        };

        self.res.reset();
        let mut ctx = Ctx {
            state: &mut self.state,
            req: &req,
            res: &mut self.res,
        };

        let result = match Route::parse(req.method, req.path) {
            Route::Health => handlers::health(&mut ctx),
            Route::Feed => handlers::feed(&mut ctx),
            Route::GetPost(id) => handlers::get_post(&mut ctx, id),
            Route::CreatePost => handlers::create_post(&mut ctx),
            Route::LikePost(id) => handlers::like_post(&mut ctx, id),
            Route::NotFound => Err(ApiError::NOT_FOUND),
        };

        let res = &mut self.res;
        if let Err(e) = result {
            res.reset();
            res.status = e.status;
            let body = ErrorBody { error: e.message };
            let _ = serde_json::to_writer(&mut res.body, &body);
        }
        res.write_to(out);

        Handled::Responded { consumed: req.len }
    }
}
