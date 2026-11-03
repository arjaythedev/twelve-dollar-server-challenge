//! The $12 server challenge feed API (SPEC.md) in safe Rust: axum on hyper, SQLite via rusqlite.
//!
//! - `api`: routes, authentication, validation and response shapes.
//! - `db`: SQLite. Reads run inline on the event loop; writes are group-committed on one writer
//!   thread with `synchronous=FULL`, so a 201 survives a power cut and fsyncs never stall reads.
//!
//! This file reads the configuration and serves HTTP/1.1 with timeouts and graceful shutdown.

mod api;
mod db;

use std::{
    env,
    error::Error,
    net::SocketAddr,
    sync::Arc,
    thread,
    time::{Duration, Instant},
};

use axum::Router;
use hmac::{Hmac, KeyInit};
use hyper::server::conn::http1;
use hyper_util::{
    rt::{TokioIo, TokioTimer},
    server::graceful::GracefulShutdown,
    service::TowerToHyperService,
};
use tokio::{net::TcpSocket, runtime::Builder};

fn main() -> Result<(), Box<dyn Error>> {
    let var = |key: &str, default: &str| env::var(key).unwrap_or_else(|_| default.to_owned());
    let db_path = env::var("SQLITE_PATH").map_err(|_| "SQLITE_PATH is not set")?;
    let secret = env::var("JWT_SECRET").map_err(|_| "JWT_SECRET is not set")?;
    let addr = SocketAddr::new(var("HOST", "127.0.0.1").parse()?, var("PORT", "3000").parse()?);
    let app = Arc::new(api::App {
        db: db::Db::open(&db_path)?,
        jwt_key: Hmac::new_from_slice(secret.as_bytes()).expect("HMAC takes keys of any length"),
        started: Instant::now(),
    });

    // One event-loop thread per core. On one core, the current-thread runtime skips work stealing.
    let threads = var("THREADS", "0").parse().ok().filter(|&n| n > 0);
    let threads = threads.unwrap_or_else(|| thread::available_parallelism().map_or(1, |n| n.get()));
    let mut runtime = if threads == 1 { Builder::new_current_thread() } else { Builder::new_multi_thread() };
    runtime.worker_threads(threads).enable_all().build()?.block_on(serve(addr, api::router(app)))
}

async fn serve(addr: SocketAddr, app: Router) -> Result<(), Box<dyn Error>> {
    let socket = if addr.is_ipv4() { TcpSocket::new_v4()? } else { TcpSocket::new_v6()? };
    #[cfg(unix)] // quick restarts past TIME_WAIT; on Windows SO_REUSEADDR would allow port stealing
    socket.set_reuseaddr(true)?;
    socket.bind(addr)?;
    let listener = socket.listen(4096)?; // the kernel caps this at net.core.somaxconn
    let mut http = http1::Builder::new();
    // hyper runs its header-read timer while a keep-alive connection waits for its next request,
    // so this is also the idle timeout: above the 65 s the spec asks for (Nginx keeps them 60 s).
    http.timer(TokioTimer::new()).header_read_timeout(Duration::from_secs(75));
    let graceful = GracefulShutdown::new();
    let mut shutdown = std::pin::pin!(shutdown_signal());
    eprintln!("listening on http://{addr}");
    loop {
        tokio::select! {
            accepted = listener.accept() => match accepted {
                Ok((stream, _)) => {
                    // A feed response is four TCP segments; don't let Nagle hold back the last one.
                    let _ = stream.set_nodelay(true);
                    let conn = http.serve_connection(TokioIo::new(stream), TowerToHyperService::new(app.clone()));
                    tokio::spawn(graceful.watch(conn));
                }
                // EMFILE and friends: back off rather than spin on a full descriptor table.
                Err(e) => {
                    eprintln!("accept: {e}");
                    tokio::time::sleep(Duration::from_millis(100)).await;
                }
            },
            _ = &mut shutdown => break,
        }
    }
    drop(listener);
    eprintln!("shutting down: finishing in-flight requests (10 s at most)");
    tokio::select! {
        _ = graceful.shutdown() => {}
        _ = tokio::time::sleep(Duration::from_secs(10)) => {}
    }
    Ok(())
}

async fn shutdown_signal() {
    let ctrl_c = tokio::signal::ctrl_c();
    #[cfg(unix)]
    {
        use tokio::signal::unix::{SignalKind, signal};
        let mut term = signal(SignalKind::terminate()).expect("install SIGTERM handler");
        tokio::select! {
            _ = ctrl_c => {}
            _ = term.recv() => {}
        }
    }
    #[cfg(not(unix))]
    let _ = ctrl_c.await;
}
