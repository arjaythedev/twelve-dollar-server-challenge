const FEED_REFERENCE_QUERY: &str = "WITH feed AS (
    SELECT p.id, p.body, p.created_at, u.username
    FROM posts p JOIN users u ON u.id = p.user_id
    ORDER BY p.created_at DESC, p.id DESC
    LIMIT 20
)
SELECT f.id, f.body, f.created_at, f.username,
    (SELECT count(*) FROM likes l WHERE l.post_id = f.id)
FROM feed f
ORDER BY f.created_at DESC, f.id DESC";

mod tests {
    use super::*;

    fn json(bytes: impl AsRef<[u8]>) -> serde_json::Value {
        serde_json::from_slice(bytes.as_ref()).unwrap()
    }

    fn test_directory() -> tempfile::TempDir {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("target/test-data");
        std::fs::create_dir_all(&root).unwrap();
        tempfile::tempdir_in(root).unwrap()
    }

    fn fixture() -> (tempfile::TempDir, std::path::PathBuf) {
        let directory = test_directory();
        let path = directory.path().join("feed.db");
        let connection = Connection::open(&path).unwrap();
        connection
            .execute_batch(include_str!("../../../../schema.sql"))
            .unwrap();
        connection
            .execute(
                "INSERT INTO users (id, username) VALUES (1, 'alice'), (2, 'bob')",
                [],
            )
            .unwrap();
        (directory, path)
    }

    async fn assert_feed_matches_sql_reference(db: &Db, path: &Path) {
        let connection = Connection::open(path).unwrap();
        let mut statement = connection.prepare(FEED_REFERENCE_QUERY).unwrap();
        let mut rows = statement.query([]).unwrap();
        let mut posts = Vec::new();
        while let Some(row) = rows.next().unwrap() {
            posts.push(
                serde_json::to_value(Post {
                    id: row.get(0).unwrap(),
                    body: row.get_ref(1).unwrap().as_str().unwrap(),
                    created_at: row.get_ref(2).unwrap().as_str().unwrap(),
                    author: row.get_ref(3).unwrap().as_str().unwrap(),
                    like_count: row.get(4).unwrap(),
                })
                .unwrap(),
            );
        }
        drop(rows);
        drop(statement);
        drop(connection);
        let expected = serde_json::json!({ "posts": posts });
        assert_eq!(json(db.feed().await.unwrap()), expected);
    }

    #[tokio::test]
    async fn feed_single_query_matches_ordered_reference_and_edge_cases() {
        let (_directory, path) = fixture();
        let db = Db::open(&path).unwrap();
        assert_feed_matches_sql_reference(&db, &path).await;

        let connection = Connection::open(&path).unwrap();
        for id in 1..=7 {
            connection
                .execute(
                    "INSERT INTO posts (id, user_id, body, created_at) VALUES (?1, 1, 'short', ?2)",
                    params![id, format!("2025-01-01T00:00:{id:02}.000Z")],
                )
                .unwrap();
        }
        drop(connection);
        assert_feed_matches_sql_reference(&db, &path).await;

        let connection = Connection::open(&path).unwrap();
        connection
            .execute_batch("DELETE FROM likes; DELETE FROM posts;")
            .unwrap();
        for id in 100..120 {
            let seconds = (id * 7) % 20;
            connection
                .execute(
                    "INSERT INTO posts (id, user_id, body, created_at) VALUES (?1, 1, ?2, ?3)",
                    params![
                        id,
                        format!("body {id}"),
                        format!("2025-01-01T00:00:{seconds:02}.000Z")
                    ],
                )
                .unwrap();
        }
        connection
            .execute(
                "INSERT INTO likes (user_id, post_id) VALUES (1, 104), (2, 104), (2, 117)",
                [],
            )
            .unwrap();
        drop(connection);
        assert_feed_matches_sql_reference(&db, &path).await;

        let connection = Connection::open(&path).unwrap();
        connection
            .execute_batch("DELETE FROM likes; DELETE FROM posts;")
            .unwrap();
        for id in (200..240).step_by(2) {
            connection
                .execute(
                    "INSERT INTO posts (id, user_id, body, created_at) VALUES (?1, 1, 'sparse', '2025-01-01T00:00:00.000Z')",
                    [id],
                )
                .unwrap();
        }
        drop(connection);
        assert_feed_matches_sql_reference(&db, &path).await;

        let connection = Connection::open(&path).unwrap();
        connection
            .execute_batch("DELETE FROM likes; DELETE FROM posts;")
            .unwrap();
        for id in 300..320 {
            connection
                .execute(
                    "INSERT INTO posts (id, user_id, body, created_at) VALUES (?1, 1, 'tied', '2025-01-01T00:00:00.000Z')",
                    [id],
                )
                .unwrap();
        }
        connection
            .execute(
                "INSERT INTO likes (user_id, post_id) VALUES (1, 303), (2, 303), (2, 305)",
                [],
            )
            .unwrap();
        drop(connection);
        assert_feed_matches_sql_reference(&db, &path).await;

        let connection = Connection::open(&path).unwrap();
        connection
            .execute_batch("DELETE FROM likes; DELETE FROM posts;")
            .unwrap();
        for id in 400..420 {
            connection
                .execute(
                    "INSERT INTO posts (id, user_id, body, created_at) VALUES (?1, 1, 'valid', '2025-01-01T00:00:00.000Z')",
                    [id],
                )
                .unwrap();
        }
        connection
            .pragma_update(None, "foreign_keys", "OFF")
            .unwrap();
        connection
            .execute(
                "INSERT INTO posts (id, user_id, body, created_at) VALUES (420, 999, 'orphan', '2026-01-01T00:00:00.000Z')",
                [],
            )
            .unwrap();
        drop(connection);
        assert_feed_matches_sql_reference(&db, &path).await;
    }

    #[tokio::test]
    async fn committed_posts_preserve_utf8_and_token_author_and_survive_reopen() {
        let (_directory, path) = fixture();
        let db = Db::open(&path).unwrap();
        let body = "héllo 🦀\n\"quoted\"";
        let created = json(
            db.create_post(1, body.into(), "token-name".into())
                .await
                .unwrap(),
        );
        let created = &created["post"];
        let id = created["id"].as_i64().unwrap();
        assert_eq!(created["body"], body);
        assert_eq!(created["author"], "token-name");
        assert_eq!(created["like_count"], 0);
        let timestamp = created["created_at"].as_str().unwrap();
        assert_eq!(timestamp.len(), 24);
        assert!(timestamp.ends_with('Z'));
        assert_eq!(db.like(2, id).await.unwrap(), Some(false));
        drop(db);

        let reopened = Db::open(&path).unwrap();
        let fetched = json(reopened.post(id).await.unwrap().unwrap());
        let fetched = &fetched["post"];
        assert_eq!(fetched["body"], body);
        assert_eq!(fetched["created_at"], timestamp);
        assert_eq!(fetched["author"], "alice");
        assert_eq!(fetched["like_count"], 1);
    }

    #[tokio::test]
    async fn committed_data_remains_available_after_reopen() {
        let (_directory, path) = fixture();
        let db = Db::open(&path).unwrap();
        let mut last_id = 0;
        for index in 0..32 {
            let created = json(
                db.create_post(1, format!("checkpoint-{index}").into(), "alice".into())
                    .await
                    .unwrap(),
            );
            last_id = created["post"]["id"].as_i64().unwrap();
        }

        drop(db);
        let reopened = Db::open(&path).unwrap();
        let fetched = json(reopened.post(last_id).await.unwrap().unwrap());
        assert_eq!(fetched["post"]["body"], "checkpoint-31");
    }

    #[test]
    fn mmap_size_reports_the_effective_value_from_the_configured_connection() {
        let (_directory, path) = fixture();
        let connection = Connection::open(path).unwrap();
        let requested_bytes = 64 * 1024 * 1024;

        let effective_bytes = configure_mmap_size(&connection, requested_bytes).unwrap();
        let queried_bytes: i64 = connection
            .query_row("PRAGMA mmap_size", [], |row| row.get(0))
            .unwrap();

        assert_eq!(effective_bytes, queried_bytes);
        assert!(effective_bytes > 0);
        assert!(effective_bytes <= requested_bytes as i64);
        assert!(matches!(
            configure_mmap_size(&connection, u64::MAX),
            Err(DbError::InvalidMmapSize)
        ));
    }

    #[tokio::test]
    async fn duplicate_likes_are_atomic_and_each_read_gets_current_data() {
        let (_directory, path) = fixture();
        let db = Db::open(&path).unwrap();
        let post = json(
            db.create_post(1, "body".into(), "alice".into())
                .await
                .unwrap(),
        );
        let id = post["post"]["id"].as_i64().unwrap();
        assert_eq!(
            json(db.post(id).await.unwrap().unwrap())["post"]["like_count"],
            0
        );
        let (first, second) = tokio::join!(db.like(2, id), db.like(2, id));
        assert_eq!((first.unwrap(), second.unwrap()), (Some(false), Some(true)));
        assert_eq!(
            json(db.post(id).await.unwrap().unwrap())["post"]["like_count"],
            1
        );
        assert_eq!(json(db.feed().await.unwrap())["posts"][0]["like_count"], 1);

        let external = Connection::open(&path).unwrap();
        external
            .execute("INSERT INTO likes (user_id, post_id) VALUES (1, ?1)", [id])
            .unwrap();
        assert_eq!(
            json(db.post(id).await.unwrap().unwrap())["post"]["like_count"],
            2
        );
        assert_eq!(json(db.feed().await.unwrap())["posts"][0]["like_count"], 2);
        assert_eq!(db.like(1, id + 1).await.unwrap(), None);
        assert!(db.post(id + 1).await.unwrap().is_none());
    }

    #[tokio::test]
    async fn feed_orders_timestamp_then_id_and_limits_to_twenty() {
        let (_directory, path) = fixture();
        let connection = Connection::open(&path).unwrap();
        for id in 1..=22 {
            connection.execute(
                "INSERT INTO posts (id, user_id, body, created_at) VALUES (?1, 1, 'body', '2025-01-01T00:00:00.000Z')",
                [id],
            ).unwrap();
        }
        connection
            .execute(
                "UPDATE posts SET created_at = '2026-01-01T00:00:00.000Z' WHERE id = 1",
                [],
            )
            .unwrap();
        let feed = json(Db::open(&path).unwrap().feed().await.unwrap());
        let feed = feed["posts"].as_array().unwrap();
        assert_eq!(feed.len(), 20);
        assert_eq!(
            feed.iter()
                .map(|post| post["id"].as_i64().unwrap())
                .collect::<Vec<_>>(),
            std::iter::once(1).chain((4..=22).rev()).collect::<Vec<_>>()
        );
    }

    #[tokio::test]
    async fn sqlite_constraints_fail_without_poisoning_the_worker() {
        let (_directory, path) = fixture();
        let db = Db::open(&path).unwrap();
        assert!(
            db.create_post(999, "body".into(), "nobody".into())
                .await
                .is_err()
        );
        assert!(
            db.create_post(1, Bytes::new(), "alice".into())
                .await
                .is_err()
        );
        assert!(matches!(
            db.create_post(1, Bytes::from_static(&[0xff]), "alice".into())
                .await,
            Err(DbError::InvalidUtf8(_))
        ));
        let post = json(
            db.create_post(1, "body".into(), "alice".into())
                .await
                .unwrap(),
        );
        let id = post["post"]["id"].as_i64().unwrap();
        assert!(db.like(999, id).await.is_err());
        assert_eq!(
            json(db.post(id).await.unwrap().unwrap())["post"]["like_count"],
            0
        );
        db.health().await.unwrap();
    }

    #[tokio::test]
    async fn borrowed_rows_produce_exact_compact_json_and_escape_strings() {
        let (_directory, path) = fixture();
        let db = Db::open(&path).unwrap();
        assert_eq!(db.feed().await.unwrap().as_ref(), br#"{"posts":[]}"#);
        let connection = Connection::open(&path).unwrap();
        connection.execute(
            "INSERT INTO posts (id, user_id, body, created_at) VALUES (1, 1, ?1, '2025-01-01T00:00:00.000Z')",
            ["héllo 🦀\n\"quoted\"\\\t"],
        ).unwrap();
        let post = db.post(1).await.unwrap().unwrap();
        assert_eq!(post, r#"{"post":{"id":1,"body":"héllo 🦀\n\"quoted\"\\\t","created_at":"2025-01-01T00:00:00.000Z","author":"alice","like_count":0}}"#.as_bytes());
        let feed = db.feed().await.unwrap();
        assert_eq!(feed, r#"{"posts":[{"id":1,"body":"héllo 🦀\n\"quoted\"\\\t","created_at":"2025-01-01T00:00:00.000Z","author":"alice","like_count":0}]}"#.as_bytes());
    }

    #[test]
    fn open_requires_existing_disk_database() {
        let directory = test_directory();
        let missing = directory.path().join("missing.db");
        assert!(Db::open(&missing).is_err());
        assert!(!missing.exists());
        assert!(matches!(Db::open(":memory:"), Err(DbError::InvalidPath)));
        assert!(matches!(Db::open(""), Err(DbError::InvalidPath)));
    }

    #[tokio::test]
    async fn closed_worker_returns_an_error() {
        let (requests, receiver) = mpsc::channel(1);
        drop(receiver);
        let db = Db {
            requests,
            worker_thread: std::thread::current(),
            admission_timings: None,
        };
        assert!(matches!(db.health().await, Err(DbError::WorkerUnavailable)));
    }

    #[tokio::test]
    async fn outstanding_requests_time_out_when_the_queue_stays_full() {
        let (requests, mut receiver) = mpsc::channel(1);
        let db = Db {
            requests,
            worker_thread: std::thread::current(),
            admission_timings: None,
        };
        let first_db = db.clone();
        let first = tokio::spawn(async move { first_db.health().await });
        let active = receiver.recv().await.unwrap();

        let second_db = db.clone();
        let second = tokio::spawn(async move { second_db.health().await });
        while db.requests.capacity() != 0 {
            tokio::task::yield_now().await;
        }

        let started = Instant::now();
        assert!(matches!(db.health().await, Err(DbError::WorkerBusy)));
        assert!(started.elapsed() >= REQUEST_ADMISSION_TIMEOUT);

        drop(active);
        drop(receiver.recv().await.unwrap());
        assert!(matches!(
            first.await.unwrap(),
            Err(DbError::WorkerUnavailable)
        ));
        assert!(matches!(
            second.await.unwrap(),
            Err(DbError::WorkerUnavailable)
        ));
        let retry = tokio::spawn({
            let db = db.clone();
            async move { db.health().await }
        });
        drop(receiver.recv().await.unwrap());
        assert!(matches!(
            retry.await.unwrap(),
            Err(DbError::WorkerUnavailable)
        ));
    }

    #[tokio::test]
    async fn read_and_write_admission_wait_for_queue_capacity() {
        for write in [false, true] {
            let (requests, mut receiver) = mpsc::channel(1);
            let db = Db {
                requests,
                worker_thread: std::thread::current(),
                admission_timings: None,
            };
            let (reply, _result) = oneshot::channel();
            db.requests.try_send(Request::Health(reply)).unwrap();
            let waiting = async {
                if write {
                    db.write_request(Request::Health).await
                } else {
                    db.health().await
                }
            };
            tokio::pin!(waiting);
            tokio::select! {
                biased;
                result = &mut waiting => panic!("full queue should wait: {result:?}"),
                _ = std::future::ready(()) => {}
            }
            drop(receiver.recv().await.unwrap());
            let (result, ()) = tokio::join!(waiting, async {
                let Request::Health(reply) = receiver.recv().await.unwrap() else {
                    panic!("expected health request");
                };
                reply.send(Ok(())).unwrap();
            });
            result.unwrap();
        }
    }

    #[tokio::test]
    async fn cancelled_queued_write_is_skipped_when_worker_drains_it() {
        let (_directory, path) = fixture();
        let db = Db::open(&path).unwrap();
        let blocker = Connection::open(&path).unwrap();
        blocker.execute_batch("BEGIN IMMEDIATE").unwrap();

        let (first_reply, first_result) = oneshot::channel();
        db.requests
            .try_send(Request::CreatePost {
                user_id: 1,
                body: "active".into(),
                username: "alice".into(),
                reply: first_reply,
            })
            .unwrap();
        while db.requests.capacity() != REQUEST_CAPACITY {
            tokio::task::yield_now().await;
        }

        let queued = tokio::spawn({
            let db = db.clone();
            async move { db.create_post(1, "cancelled".into(), "alice".into()).await }
        });
        while db.requests.capacity() != REQUEST_CAPACITY - 1 {
            tokio::task::yield_now().await;
        }

        queued.abort();
        let _ = queued.await;
        blocker.execute_batch("ROLLBACK").unwrap();
        first_result.await.unwrap().unwrap();
        db.health().await.unwrap();

        let connection = Connection::open(&path).unwrap();
        let posts: Vec<String> = connection
            .prepare("SELECT body FROM posts ORDER BY id")
            .unwrap()
            .query_map([], |row| row.get(0))
            .unwrap()
            .collect::<rusqlite::Result<_>>()
            .unwrap();
        assert_eq!(posts, ["active"]);
    }

    #[test]
    fn read_batch_stops_at_write_and_preserves_fifo_order() {
        let (sender, mut receiver) = mpsc::channel(8);
        let (reply, _response) = oneshot::channel();
        sender.try_send(Request::Post(1, reply)).unwrap();
        let (reply, _response) = oneshot::channel();
        sender.try_send(Request::Feed(reply)).unwrap();
        let (reply, _response) = oneshot::channel();
        sender
            .try_send(Request::CreatePost {
                user_id: 7,
                body: Bytes::new(),
                username: String::new(),
                reply,
            })
            .unwrap();
        let (reply, _response) = oneshot::channel();
        sender.try_send(Request::Post(3, reply)).unwrap();

        let first = receiver.try_recv().unwrap();
        let mut batch = std::array::from_fn(|_| None);
        let (len, pending) = collect_read_batch(first, &mut receiver, &mut batch);

        assert_eq!(len, 2);
        assert!(matches!(batch[0], Some(Request::Post(1, _))));
        assert!(matches!(batch[1], Some(Request::Feed(_))));
        assert!(matches!(
            pending,
            Some(Request::CreatePost { user_id: 7, .. })
        ));
        assert!(matches!(receiver.try_recv(), Ok(Request::Post(3, _))));
    }

    #[test]
    fn read_batch_is_bounded_and_never_waits_for_more_requests() {
        let (sender, mut receiver) = mpsc::channel(READ_BATCH_LIMIT + 2);
        for id in 0..(READ_BATCH_LIMIT + 2) {
            let (reply, _response) = oneshot::channel();
            sender.try_send(Request::Post(id as i64, reply)).unwrap();
        }

        let first = receiver.try_recv().unwrap();
        let mut batch = std::array::from_fn(|_| None);
        let (len, pending) = collect_read_batch(first, &mut receiver, &mut batch);

        assert_eq!(len, READ_BATCH_LIMIT);
        assert!(pending.is_none());
        assert!(matches!(batch[0], Some(Request::Post(0, _))));
        assert!(matches!(
            batch[READ_BATCH_LIMIT - 1],
            Some(Request::Post(id, _)) if id == READ_BATCH_LIMIT as i64 - 1
        ));
        assert!(
            matches!(receiver.try_recv(), Ok(Request::Post(id, _)) if id == READ_BATCH_LIMIT as i64)
        );
        assert!(
            matches!(receiver.try_recv(), Ok(Request::Post(id, _)) if id == READ_BATCH_LIMIT as i64 + 1)
        );

        // An empty queue ends collection when the bounded collection window
        // expires; no later request is needed to complete the batch.
        let (reply, _response) = oneshot::channel();
        let (len, pending) =
            collect_read_batch(Request::Post(10, reply), &mut receiver, &mut batch);
        assert_eq!(len, 1);
        assert!(pending.is_none());
    }

    #[test]
    fn write_batch_is_bounded_and_keeps_mixed_fifo_snapshot() {
        let (sender, mut receiver) = mpsc::channel(WRITE_BATCH_LIMIT + 2);
        let mut responses = Vec::new();
        for index in 0..(WRITE_BATCH_LIMIT + 1) {
            let (reply, response) = oneshot::channel();
            responses.push(response);
            sender
                .try_send(Request::Like {
                    user_id: 1,
                    post_id: index as i64,
                    reply,
                })
                .unwrap();
        }
        let first = receiver.try_recv().unwrap();
        let mut batch = std::array::from_fn(|_| None);
        let len = collect_write_batch(first, &mut receiver, &mut batch);
        assert_eq!(len, WRITE_BATCH_LIMIT);
        assert!(matches!(batch[0], Some(Request::Like { post_id: 0, .. })));
        assert!(matches!(
            batch[WRITE_BATCH_LIMIT - 1],
            Some(Request::Like { post_id, .. }) if post_id == WRITE_BATCH_LIMIT as i64 - 1
        ));
        assert!(matches!(
            receiver.try_recv(),
            Ok(Request::Like { post_id, .. }) if post_id == WRITE_BATCH_LIMIT as i64
        ));
        drop(sender);
        drop(responses);
    }

    #[test]
    fn write_batch_preserves_valid_writes_around_item_failure_and_read_order() {
        let (_directory, path) = fixture();
        let connection = Connection::open(&path).unwrap();
        let mut statements = WorkerStatements::prepare(&connection).unwrap();
        let mut batch: [Option<Request>; WRITE_BATCH_LIMIT] = std::array::from_fn(|_| None);
        let (first_reply, first_response) = oneshot::channel();
        let (bad_reply, bad_response) = oneshot::channel();
        let (feed_reply, feed_response) = oneshot::channel();
        let (last_reply, last_response) = oneshot::channel();
        batch[0] = Some(Request::CreatePost {
            user_id: 1,
            body: "first".into(),
            username: "alice".into(),
            reply: first_reply,
        });
        batch[1] = Some(Request::CreatePost {
            user_id: 1,
            body: Bytes::from_static(b"\xff"),
            username: "alice".into(),
            reply: bad_reply,
        });
        batch[2] = Some(Request::Feed(feed_reply));
        batch[3] = Some(Request::CreatePost {
            user_id: 2,
            body: "last".into(),
            username: "bob".into(),
            reply: last_reply,
        });
        let mut feed_capacity = 4096;
        let mut post_capacity = 256;
        let mut created_post_capacity = 256;

        assert!(process_write_batch(
            &connection,
            &mut statements,
            &mut batch[..4],
            WriteBatchCapacities {
                feed: &mut feed_capacity,
                post: &mut post_capacity,
                created_post: &mut created_post_capacity,
            },
            false,
            &mut std::array::from_fn(|_| None),
        ));
        assert_eq!(
            json(first_response.blocking_recv().unwrap().unwrap())["post"]["body"],
            "first"
        );
        assert!(matches!(
            bad_response.blocking_recv().unwrap(),
            Err(DbError::InvalidUtf8(_))
        ));
        let feed = json(feed_response.blocking_recv().unwrap().unwrap());
        assert_eq!(feed["posts"][0]["body"], "first");
        assert_eq!(
            json(last_response.blocking_recv().unwrap().unwrap())["post"]["body"],
            "last"
        );
        let count: i64 = connection
            .query_row("SELECT count(*) FROM posts", [], |row| row.get(0))
            .unwrap();
        assert_eq!(count, 2);
    }

    #[test]
    fn write_batch_commit_failure_rolls_back_all_successful_items() {
        let (_directory, path) = fixture();
        let connection = Connection::open(&path).unwrap();
        let mut statements = WorkerStatements::prepare(&connection).unwrap();
        statements.commit_write = connection
            .prepare("SELECT abs(-9223372036854775808)")
            .unwrap();
        let mut batch: [Option<Request>; WRITE_BATCH_LIMIT] = std::array::from_fn(|_| None);
        let (reply, response) = oneshot::channel();
        batch[0] = Some(Request::CreatePost {
            user_id: 1,
            body: "rolled back".into(),
            username: "alice".into(),
            reply,
        });
        let mut feed_capacity = 4096;
        let mut post_capacity = 256;
        let mut created_post_capacity = 256;

        assert!(process_write_batch(
            &connection,
            &mut statements,
            &mut batch[..1],
            WriteBatchCapacities {
                feed: &mut feed_capacity,
                post: &mut post_capacity,
                created_post: &mut created_post_capacity,
            },
            false,
            &mut std::array::from_fn(|_| None),
        ));
        assert!(
            matches!(response.blocking_recv().unwrap(), Err(DbError::WriteBatch(message)) if message.starts_with("commit failed:"))
        );
        let count: i64 = connection
            .query_row("SELECT count(*) FROM posts", [], |row| row.get(0))
            .unwrap();
        assert_eq!(count, 0);
        assert!(connection.is_autocommit());
    }

    #[tokio::test]
    async fn cancelled_queued_read_is_skipped_before_the_next_read() {
        let (_directory, path) = fixture();
        let db = Db::open(&path).unwrap();
        let blocker = Connection::open(&path).unwrap();
        blocker.execute_batch("BEGIN IMMEDIATE").unwrap();

        let (active_reply, active_result) = oneshot::channel();
        db.requests
            .try_send(Request::CreatePost {
                user_id: 1,
                body: "active".into(),
                username: "alice".into(),
                reply: active_reply,
            })
            .unwrap();
        while db.requests.capacity() != REQUEST_CAPACITY {
            tokio::task::yield_now().await;
        }

        let (cancelled_reply, cancelled_result) = oneshot::channel();
        db.requests
            .try_send(Request::Post(1, cancelled_reply))
            .unwrap();
        drop(cancelled_result);
        let (live_reply, live_result) = oneshot::channel();
        db.requests.try_send(Request::Post(1, live_reply)).unwrap();
        while db.requests.capacity() != REQUEST_CAPACITY - 2 {
            tokio::task::yield_now().await;
        }

        blocker.execute_batch("ROLLBACK").unwrap();
        active_result.await.unwrap().unwrap();
        assert!(live_result.await.unwrap().unwrap().is_some());
        db.health().await.unwrap();
    }

    #[tokio::test]
    async fn read_batch_keeps_each_request_response_independent() {
        let (_directory, path) = fixture();
        let connection = Connection::open(&path).unwrap();
        connection
            .pragma_update(None, "ignore_check_constraints", "ON")
            .unwrap();
        connection
            .execute(
                "INSERT INTO posts (id, user_id, body, created_at) VALUES (1, 1, ?1, '2025-01-01T00:00:00.000Z'), (2, 2, 'two', '2025-01-02T00:00:00.000Z')",
                [rusqlite::types::Value::Blob(vec![0xff])],
            )
            .unwrap();
        let mut statements = WorkerStatements::prepare(&connection).unwrap();
        let mut batch = std::array::from_fn(|_| None);
        let (first_reply, first_response) = oneshot::channel();
        let (second_reply, second_response) = oneshot::channel();
        let (health_reply, health_response) = oneshot::channel();
        batch[0] = Some(Request::Post(1, first_reply));
        batch[1] = Some(Request::Post(2, second_reply));
        batch[2] = Some(Request::Health(health_reply));
        let mut feed_capacity = 4096;
        let mut post_capacity = 256;

        assert!(process_read_batch(
            &connection,
            &mut statements,
            &mut batch,
            3,
            &mut feed_capacity,
            &mut post_capacity,
            &mut std::array::from_fn(|_| None),
        ));

        assert!(matches!(
            first_response.await.unwrap(),
            Err(DbError::Sqlite(_))
        ));
        let second = json(second_response.await.unwrap().unwrap().unwrap());
        assert_eq!(second["post"]["id"], 2);
        assert_eq!(second["post"]["body"], "two");
        health_response.await.unwrap().unwrap();
        assert!(connection.is_autocommit());
    }

    #[test]
    fn read_batch_begin_error_is_reported_without_rolling_back_existing_transaction() {
        let (_directory, path) = fixture();
        let connection = Connection::open(&path).unwrap();
        let mut statements = WorkerStatements::prepare(&connection).unwrap();
        connection.execute_batch("BEGIN").unwrap();
        let mut batch = std::array::from_fn(|_| None);
        let (first_reply, first_response) = oneshot::channel();
        let (second_reply, second_response) = oneshot::channel();
        batch[0] = Some(Request::Health(first_reply));
        batch[1] = Some(Request::Health(second_reply));
        let mut feed_capacity = 4096;
        let mut post_capacity = 256;

        let reusable = process_read_batch(
            &connection,
            &mut statements,
            &mut batch,
            2,
            &mut feed_capacity,
            &mut post_capacity,
            &mut std::array::from_fn(|_| None),
        );

        for response in [first_response, second_response] {
            let Err(DbError::ReadBatch(message)) = response.blocking_recv().unwrap() else {
                panic!("begin failure should be reported to each read");
            };
            assert!(message.starts_with("begin failed:"));
        }
        assert!(!connection.is_autocommit());
        assert!(!reusable);
        connection.execute_batch("ROLLBACK").unwrap();
        assert!(connection.is_autocommit());
    }

    #[test]
    fn read_batch_commit_error_is_reported_after_transaction_is_rolled_back() {
        let (_directory, path) = fixture();
        let connection = Connection::open(&path).unwrap();
        let mut statements = WorkerStatements::prepare(&connection).unwrap();
        let transaction = ReadBatchTransaction::begin(
            &connection,
            &mut statements.begin_read,
            &mut statements.rollback_read,
        )
        .unwrap();
        connection.execute_batch("ROLLBACK").unwrap();

        let error = transaction.commit(&mut statements.commit_read).unwrap_err();

        assert!(error.starts_with("commit failed:"));
        assert!(connection.is_autocommit());
    }

    #[test]
    fn read_batch_stops_using_connection_when_commit_and_rollback_fail() {
        let (_directory, path) = fixture();
        let connection = Connection::open(&path).unwrap();
        let mut statements = WorkerStatements::prepare(&connection).unwrap();
        statements.commit_read = connection
            .prepare("SELECT abs(-9223372036854775808)")
            .unwrap();
        statements.rollback_read = connection
            .prepare("SELECT abs(-9223372036854775808)")
            .unwrap();
        let mut batch = std::array::from_fn(|_| None);
        let (first_reply, first_response) = oneshot::channel();
        let (second_reply, second_response) = oneshot::channel();
        batch[0] = Some(Request::Health(first_reply));
        batch[1] = Some(Request::Health(second_reply));
        let mut feed_capacity = 4096;
        let mut post_capacity = 256;

        let reusable = process_read_batch(
            &connection,
            &mut statements,
            &mut batch,
            2,
            &mut feed_capacity,
            &mut post_capacity,
            &mut std::array::from_fn(|_| None),
        );

        for response in [first_response, second_response] {
            let Err(DbError::ReadBatch(message)) = response.blocking_recv().unwrap() else {
                panic!("failed controls should report the commit error to each read");
            };
            assert!(message.starts_with("commit failed:"));
            assert!(message.contains("rollback failed:"));
        }
        assert!(!reusable);
        assert!(!connection.is_autocommit());
        connection.execute_batch("ROLLBACK").unwrap();
    }

    #[test]
    fn read_batch_begin_error_with_autocommit_connection_is_recoverable() {
        let (_directory, path) = fixture();
        let connection = Connection::open(&path).unwrap();
        let mut statements = WorkerStatements::prepare(&connection).unwrap();
        statements.begin_read = connection
            .prepare("SELECT abs(-9223372036854775808)")
            .unwrap();
        let mut batch = std::array::from_fn(|_| None);
        let (reply, response) = oneshot::channel();
        batch[0] = Some(Request::Health(reply));
        let mut feed_capacity = 4096;
        let mut post_capacity = 256;

        let reusable = process_read_batch(
            &connection,
            &mut statements,
            &mut batch,
            1,
            &mut feed_capacity,
            &mut post_capacity,
            &mut std::array::from_fn(|_| None),
        );

        let Err(DbError::ReadBatch(message)) = response.blocking_recv().unwrap() else {
            panic!("failed begin control should report an error");
        };
        assert!(message.starts_with("begin failed:"));
        assert!(reusable);
        assert!(connection.is_autocommit());
    }

    #[test]
    fn read_batch_transaction_drop_rolls_back_during_unwind() {
        let (_directory, path) = fixture();
        let connection = Connection::open(&path).unwrap();
        let mut statements = WorkerStatements::prepare(&connection).unwrap();

        let panic = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let _transaction = ReadBatchTransaction::begin(
                &connection,
                &mut statements.begin_read,
                &mut statements.rollback_read,
            )
            .unwrap();
            panic!("simulated read batch panic");
        }));

        assert!(panic.is_err());
        assert!(connection.is_autocommit());
    }

    #[test]
    fn open_reports_fixed_statement_prepare_errors() {
        let directory = test_directory();
        let path = directory.path().join("empty.db");
        Connection::open(&path).unwrap();

        assert!(matches!(Db::open(path), Err(DbError::Sqlite(_))));
    }
}
