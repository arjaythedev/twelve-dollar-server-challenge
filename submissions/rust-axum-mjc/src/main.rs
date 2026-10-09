use std::{env, error::Error, net::IpAddr};
use twelve_dollar_server::{auth::Auth, db::Db, web};

#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<(), Box<dyn Error>> {
    let database = Db::open(env::var("SQLITE_PATH")?)?;
    let auth = Auth::new(env::var("JWT_SECRET")?);
    let host: IpAddr = env::var("HOST")
        .unwrap_or_else(|_| "127.0.0.1".into())
        .parse()?;
    let port: u16 = env::var("PORT").unwrap_or_else(|_| "3000".into()).parse()?;
    let listener = tokio::net::TcpListener::bind((host, port)).await?;
    eprintln!("listening on {}", listener.local_addr()?);
    axum::serve(listener, web::router(database, auth))
        .with_graceful_shutdown(shutdown())
        .await?;
    Ok(())
}

async fn shutdown() {
    #[cfg(unix)]
    {
        let mut terminate =
            tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
                .expect("install SIGTERM handler");
        tokio::select! {
            _ = tokio::signal::ctrl_c() => {},
            _ = terminate.recv() => {},
        }
    }
    #[cfg(not(unix))]
    let _ = tokio::signal::ctrl_c().await;
}
