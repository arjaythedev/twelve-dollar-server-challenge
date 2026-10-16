use crate::web::Response;
use axum::http::Request;
#[cfg(not(target_os = "linux"))]
use futures_util::FutureExt;
#[cfg(not(target_os = "linux"))]
use hyper::server::conn::http1;
use hyper::{body::Incoming, service::Service};
#[cfg(not(target_os = "linux"))]
use hyper_util::{rt::TokioIo, server::graceful::GracefulShutdown};
#[cfg(any(test, not(target_os = "linux")))]
use std::time::Duration;
use std::{
    convert::Infallible,
    future::{Future, poll_fn},
    io,
    task::Poll,
};
use tokio::net::TcpListener;

#[cfg(target_os = "linux")]
mod idle;
#[cfg(target_os = "linux")]
mod socket;

pub async fn serve<S, F>(listener: TcpListener, service: S, shutdown: F) -> io::Result<()>
where
    S: Service<Request<Incoming>, Response = Response, Error = Infallible> + Clone + Send + 'static,
    S::Future: Send + 'static,
    F: Future<Output = ()>,
{
    serve_many(vec![listener], service, shutdown).await
}

/// Serves on all listeners with one shared service and graceful shutdown.
pub async fn serve_many<S, F>(
    listeners: Vec<TcpListener>,
    service: S,
    shutdown: F,
) -> io::Result<()>
where
    S: Service<Request<Incoming>, Response = Response, Error = Infallible> + Clone + Send + 'static,
    S::Future: Send + 'static,
    F: Future<Output = ()>,
{
    if listeners.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "at least one TCP listener is required",
        ));
    }

    #[cfg(target_os = "linux")]
    return idle::serve_many(listeners, service, shutdown).await;

    #[cfg(not(target_os = "linux"))]
    serve_many_registered(listeners, service, shutdown).await
}

#[cfg(not(target_os = "linux"))]
async fn serve_many_registered<S, F>(
    listeners: Vec<TcpListener>,
    service: S,
    shutdown: F,
) -> io::Result<()>
where
    S: Service<Request<Incoming>, Response = Response, Error = Infallible> + Clone + Send + 'static,
    S::Future: Send + 'static,
    F: Future<Output = ()>,
{
    let mut http = http1::Builder::new();
    // Match Axum's timer-free HTTP/1 configuration and avoid vectored writes.
    http.header_read_timeout(None).writev(false);
    let graceful = GracefulShutdown::new();
    tokio::pin!(shutdown);
    let mut next_listener = 0;
    loop {
        let (stream, _) = tokio::select! {
            biased;
            _ = &mut shutdown => break,
            accepted = accept_any(&listeners, &mut next_listener) => match accepted {
                Ok(connection) => connection,
                Err(error) => {
                    if accept_error_action(&error) == AcceptErrorAction::Backoff {
                        eprintln!("failed to accept connection: {error}");
                        // Resource exhaustion may clear once connections close.
                        // Let shutdown interrupt the delay before retrying.
                        if wait_for_accept_backoff_or_shutdown(&mut shutdown).await {
                            break;
                        }
                    }
                    continue;
                }
            },
        };
        if let Err(error) = stream.set_nodelay(true) {
            eprintln!("failed to set TCP_NODELAY on incoming connection: {error}");
        }
        let service = service.clone();
        let connection = graceful.watch(http.serve_connection(TokioIo::new(stream), service));
        tokio::spawn(connection.map(|result| {
            if let Err(error) = result {
                eprintln!("connection failed: {error}");
            }
        }));
    }
    drop(listeners);
    graceful.shutdown().await;
    Ok(())
}

async fn accept_any(
    listeners: &[TcpListener],
    next_listener: &mut usize,
) -> io::Result<(tokio::net::TcpStream, std::net::SocketAddr)> {
    poll_fn(|cx| {
        let count = listeners.len();
        let start = *next_listener;
        for offset in 0..count {
            let index = (start + offset) % count;
            if let Poll::Ready(accepted) = listeners[index].poll_accept(cx) {
                *next_listener = (index + 1) % count;
                return Poll::Ready(accepted);
            }
        }
        Poll::Pending
    })
    .await
}

#[derive(Debug, PartialEq, Eq)]
enum AcceptErrorAction {
    RetryImmediately,
    Backoff,
}

fn accept_error_action(error: &io::Error) -> AcceptErrorAction {
    match error.kind() {
        io::ErrorKind::Interrupted
        | io::ErrorKind::WouldBlock
        | io::ErrorKind::ConnectionRefused
        | io::ErrorKind::ConnectionAborted
        | io::ErrorKind::ConnectionReset
        | io::ErrorKind::NetworkDown
        | io::ErrorKind::HostUnreachable
        | io::ErrorKind::NetworkUnreachable
        | io::ErrorKind::Unsupported => return AcceptErrorAction::RetryImmediately,
        _ => {}
    }

    #[cfg(target_os = "linux")]
    if matches!(
        error.raw_os_error(),
        Some(libc::ENETDOWN)
            | Some(libc::EPROTO)
            | Some(libc::ENOPROTOOPT)
            | Some(libc::EHOSTDOWN)
            | Some(libc::ENONET)
            | Some(libc::EHOSTUNREACH)
            | Some(libc::EOPNOTSUPP)
            | Some(libc::ENETUNREACH)
    ) {
        return AcceptErrorAction::RetryImmediately;
    }

    AcceptErrorAction::Backoff
}

#[cfg(any(test, not(target_os = "linux")))]
async fn wait_for_accept_backoff_or_shutdown<F>(shutdown: &mut F) -> bool
where
    F: Future<Output = ()> + Unpin,
{
    tokio::select! {
        biased;
        _ = &mut *shutdown => true,
        _ = tokio::time::sleep(Duration::from_secs(1)) => false,
    }
}

#[cfg(test)]
include!("tests/server.rs");
