//! Startup configuration. SPEC.md runtime contract: env only.

#[derive(Debug, Clone)]
pub struct Config {
    pub sqlite_path: String,
    pub jwt_secret: String,
    pub host: String,
    pub port: u16,
}

impl Config {
    /// Read from the environment; a missing required var aborts startup
    /// (rule 10: healthy within 60 s — failing fast is the honest failure).
    pub fn from_env() -> Result<Self, String> {
        let sqlite_path = std::env::var("SQLITE_PATH").map_err(|_| "SQLITE_PATH missing")?;
        let jwt_secret = std::env::var("JWT_SECRET").map_err(|_| "JWT_SECRET missing")?;
        let host = std::env::var("HOST").unwrap_or_else(|_| "0.0.0.0".into());
        let port = std::env::var("PORT").unwrap_or_else(|_| "80".into());
        let port: u16 = port.parse().map_err(|e| format!("PORT invalid: {e}"))?;
        Ok(Self { sqlite_path, jwt_secret, host, port })
    }
}
