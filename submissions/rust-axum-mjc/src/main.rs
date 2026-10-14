use std::{env, error::Error};
use twelve_dollar_server::{auth::Auth, bind, db::Db, server, web};

fn main() -> Result<(), Box<dyn Error>> {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()?
        .block_on(run())
}

async fn run() -> Result<(), Box<dyn Error>> {
    let addresses = bind::parse_addresses(
        &env::var("HOST").unwrap_or_else(|_| "127.0.0.1".into()),
        &env::var("PORT").unwrap_or_else(|_| "3000".into()),
    )?;
    let read_batching = env::var("READ_BATCHING")
        .unwrap_or_else(|_| "true".into())
        .parse::<bool>()?;
    let mmap_size_bytes = env::var("SQLITE_MMAP_SIZE")
        .unwrap_or_else(|_| "0".into())
        .parse::<u64>()?;
    let database = Db::open_with_read_batching_and_mmap_size(
        env::var("SQLITE_PATH")?,
        read_batching,
        mmap_size_bytes,
    )?;
    let auth = Auth::new(env::var("JWT_SECRET")?);
    let mut listeners = Vec::with_capacity(addresses.len());
    for address in addresses {
        listeners.push(tokio::net::TcpListener::bind(address).await?);
    }
    for listener in &listeners {
        eprintln!("listening on {}", listener.local_addr()?);
    }
    server::serve_many(listeners, web::hyper_router(database, auth), shutdown()).await?;
    Ok(())
}

async fn shutdown() {
    #[cfg(unix)]
    {
        tokio::select! {
            _ = tokio::signal::ctrl_c() => {},
            _ = async {
                use tokio::signal::unix::{SignalKind, signal};
                signal(SignalKind::terminate()).expect("install SIGTERM handler").recv().await;
            } => {},
        }
    }
    #[cfg(not(unix))]
    let _ = tokio::signal::ctrl_c().await;
}
