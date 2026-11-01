use super::*;

#[test]
#[ignore = "requires a disposable seeded SQLITE_PATH and CHECK_TOKEN"]
fn responses_follow_commit_and_failed_batches_leave_no_rows() {
    let _fixture = crate::SEEDED_TEST.lock().unwrap();
    let path = std::env::temp_dir().join(format!("writer-commit-{}.db", std::process::id()));
    std::fs::copy(std::env::var("SQLITE_PATH").unwrap(), &path).unwrap();
    let body = r#"{"body":"writer commit check"}"#;
    let raw = format!(
        "POST /posts HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer {}\r\nContent-Length: {}\r\n\r\n{}",
        std::env::var("CHECK_TOKEN").unwrap(),
        body.len(),
        body
    );
    let mut last_id = 0;
    for reject in [false, true] {
        let database = path.clone();
        let writer = Writer::spawn(move || {
            let mut db = db::Db::open(database.to_str().unwrap(), true);
            if reject {
                db.reject_commits();
            }
            App {
                db,
                auth: Auth::new("twelve-dollar-challenge"),
                start: Instant::now(),
            }
        })
        .unwrap();
        for fd in 0..64 {
            writer
                .sender
                .tx
                .send(WriteJob {
                    fd,
                    generation: 73,
                    request: raw.as_bytes().to_vec(),
                    ..Default::default()
                })
                .unwrap();
        }
        writer.sender.thread.unpark();
        let mut reader = db::Db::open(path.to_str().unwrap(), false);
        for fd in 0..64 {
            let job = writer
                .replies
                .as_ref()
                .unwrap()
                .recv_timeout(Duration::from_secs(30))
                .unwrap();
            assert_eq!((job.fd, job.generation), (fd, 73));
            assert_eq!(job.status, if reject { 500 } else { 201 });
            if !reject {
                let value: serde_json::Value = serde_json::from_slice(&job.body).unwrap();
                last_id = value["post"]["id"].as_i64().unwrap();
                // A separate connection must see every published row already.
                assert_eq!(reader.read(Some(last_id), &mut Vec::new()), 200);
            }
        }
        drop(writer);
        if reject {
            assert_eq!(reader.read(Some(last_id + 1), &mut Vec::new()), 404);
        }
        drop(reader);
    }
    std::fs::remove_file(path).unwrap();
}
