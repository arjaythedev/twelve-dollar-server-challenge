mod tests {
    use super::*;

    fn json(bytes: Vec<u8>) -> serde_json::Value {
        serde_json::from_slice(&bytes).unwrap()
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
            .execute_batch(include_str!("../../../schema.sql"))
            .unwrap();
        connection
            .execute(
                "INSERT INTO users (id, username) VALUES (1, 'alice'), (2, 'bob')",
                [],
            )
            .unwrap();
        (directory, path)
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
        assert_eq!(db.feed().await.unwrap(), br#"{"posts":[]}"#);
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
        let db = Db { requests };
        assert!(matches!(db.health().await, Err(DbError::WorkerUnavailable)));
    }
}
