//! Server-side store: the rusqlite writer, the pure-Rust mmap reader, and
//! the degradation ladder (REQ-URIK-R7): any reader fault falls back to the
//! bundled-SQLite path for that request; the server never returns wrong
//! data and never dies on reader faults.
//!
//! Pass protocol (ADR 0003/0004): reads go through the reader at the current
//! snapshot; writes batch through rusqlite in one transaction; COMMIT lands
//! before the pass's responses flush; reader.refresh() advances the snapshot
//! at the start of the NEXT pass.

use crate::jwt::KeyedMac;
use rusqlite::Connection;
use sqlite_btree::store::Store as ReaderStore;

pub struct Roots {
    pub users: u32,
    pub posts: u32,
    pub likes_idx: u32,
}

pub struct Store {
    pub writer: Connection,
    pub reader: Option<ReaderStore>,
    pub roots: Roots,
    pub started: std::time::Instant,
    /// Keyed once at startup (P3: midstate reuse); cloned per request.
    pub keyed_mac: crate::jwt::KeyedMac,
    pub jwt_secret: String,
    pub fallback_reads: std::cell::Cell<u64>,
}

/// One decoded post row (reader path or fallback path — same shape).
#[derive(Debug, Clone)]
pub struct PostRow {
    pub id: i64,
    pub user_id: i64,
    pub body: Vec<u8>,
    pub created_at: Vec<u8>,
}

fn io_or_corrupt(_e: std::io::Error) -> sqlite_btree::ReaderError {
    sqlite_btree::ReaderError::Corrupt("io error")
}

impl Store {
    /// Open writer + reader; verify the exact schema layout (refuse to start
    /// on mismatch — the reader decodes fixed column orders).
    pub fn open(path: &str, jwt_secret: &str) -> Result<Self, String> {
        let writer = Connection::open(path).map_err(|e| e.to_string())?;
        writer.pragma_update(None, "journal_mode", "WAL").map_err(|e| e.to_string())?;
        writer.pragma_update(None, "synchronous", "NORMAL").map_err(|e| e.to_string())?;
        writer.pragma_update(None, "wal_autocheckpoint", 0).map_err(|e| e.to_string())?;
        writer.pragma_update(None, "temp_store", "MEMORY").map_err(|e| e.to_string())?;

        // layout verification: column order must match what the reader decodes
        let expect: &[(&str, &str)] = &[
            ("users", ",id,username,created_at"),
            ("posts", ",id,user_id,body,created_at"),
            ("likes", ",user_id,post_id,created_at"),
        ];
        for (table, want) in expect {
            let mut st = writer
                .prepare(&format!("PRAGMA table_info({table})"))
                .map_err(|e| e.to_string())?;
            let mut got = String::new();
            let mut rows = st.query([]).map_err(|e| e.to_string())?;
            while let Some(r) = rows.next().map_err(|e| e.to_string())? {
                let name: String = r.get(1).map_err(|e| e.to_string())?;
                got.push(',');
                got.push_str(&name);
            }
            if got != *want {
                return Err(format!("layout mismatch for {table}: got [{got}] want [{want}]"));
            }
        }

        let mut roots = Roots { users: 0, posts: 0, likes_idx: 0 };
        {
            let mut st = writer
                .prepare("SELECT name, rootpage FROM sqlite_schema WHERE name IN ('users','posts','likes_post_id_idx')")
                .map_err(|e| e.to_string())?;
            let mut rows = st.query([]).map_err(|e| e.to_string())?;
            while let Some(r) = rows.next().map_err(|e| e.to_string())? {
                let name: String = r.get(0).map_err(|e| e.to_string())?;
                let page: i64 = r.get(1).map_err(|e| e.to_string())?;
                match name.as_str() {
                    "users" => roots.users = page as u32,
                    "posts" => roots.posts = page as u32,
                    "likes_post_id_idx" => roots.likes_idx = page as u32,
                    _ => {}
                }
            }
        }
        if roots.users == 0 || roots.posts == 0 || roots.likes_idx == 0 {
            return Err("rootpage lookup failed".into());
        }

        let reader = Store::open_reader(path).ok();
        Ok(Self {
            writer,
            reader,
            roots,
            started: std::time::Instant::now(),
            keyed_mac: KeyedMac::new(jwt_secret.as_bytes()),
            jwt_secret: jwt_secret.to_string(),
            fallback_reads: std::cell::Cell::new(0),
        })
    }

    fn open_reader(path: &str) -> Result<ReaderStore, sqlite_btree::ReaderError> {
        sqlite_btree::store::Store::open(path)
    }

    /// Reader access; None once a fatal reader fault switched the store to
    /// full-fallback mode (all reads via rusqlite for the rest of the run).
    pub fn reader(&self) -> Option<&ReaderStore> {
        self.reader.as_ref()
    }

    /// Fatal reader fault: drop the reader permanently.
    pub fn disable_reader(&mut self) {
        self.reader = None;
        eprintln!("store: reader disabled; all reads via bundled sqlite (fallback)");
    }

    pub fn note_fallback(&self) {
        self.fallback_reads.set(self.fallback_reads.get() + 1);
    }

    /// Advance the reader snapshot to the WAL end after write batches
    /// committed through the writer connection.
    pub fn refresh_snapshot(&mut self) {
        if let Some(r) = self.reader.as_mut() {
            let before = r.snapshot();
            if let Err(e) = r.refresh() {
                eprintln!("store: reader refresh failed ({e:?}); disabling reader");
                self.reader = None;
                return;
            }
            if std::env::var_os("LOOP_DEBUG").is_some() {
                eprintln!("refresh: snapshot {} -> {}", before, r.snapshot());
            }
        }
    }

    pub fn writer_conn(&self) -> &Connection {
        &self.writer
    }

    pub fn health_db(&self) -> bool {
        self.writer
            .query_row("SELECT 1", [], |_| Ok(()))
            .is_ok()
    }

    pub fn uptime_s(&self) -> u64 {
        self.started.elapsed().as_secs()
    }
}
