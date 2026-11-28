//! The $12 Server Challenge submission binary.
//! Config comes only from env (SPEC.md): SQLITE_PATH, JWT_SECRET, HOST, PORT.

pub mod config;
pub mod handlers;
pub mod jwt;
pub mod router;
pub mod loop_;
pub mod store;

fn main() {
    let cfg = config::Config::from_env().unwrap_or_else(|e| {
        eprintln!("config: {e}");
        std::process::exit(2);
    });
    let mut server = loop_::Server::new(cfg.clone()).unwrap_or_else(|e| {
        eprintln!("server init: {e}");
        std::process::exit(1);
    });
    eprintln!(
        "feed-server: sqlite={} listening on {}:{}",
        cfg.sqlite_path, cfg.host, cfg.port
    );
    if let Err(e) = server.run() {
        eprintln!("event loop failed: {e}");
        std::process::exit(1);
    }
}
