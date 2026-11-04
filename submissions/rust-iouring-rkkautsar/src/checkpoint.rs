use std::thread;
use std::time::Duration;

use crate::db::Db;

pub fn spawn(path: String) {
    thread::Builder::new()
        .name("checkpoint".into())
        .spawn(move || {
        let db = match Db::open(&path) {
            Ok(db) => db,
            Err(e) => {
                eprintln!("checkpointer open failed: {e}");
                return;
            }
        };
        loop {
            thread::sleep(Duration::from_millis(100));
            let _ = db.exec("PRAGMA wal_checkpoint(PASSIVE)");
        }
    })
    .expect("spawn checkpoint");
}
