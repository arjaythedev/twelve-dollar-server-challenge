mod tests {
    use super::*;
    use axum::{
        body::Body,
        http::{Request, header::CONTENT_TYPE},
    };
    use http_body_util::BodyExt;
    use jsonwebtoken::{Algorithm, EncodingKey, Header, encode};
    use serde_json::{Value, json};
    use std::{
        path::PathBuf,
        time::{SystemTime, UNIX_EPOCH},
    };
    use tempfile::TempDir;
    use tower::ServiceExt;

    struct Fixture {
        app: Router,
        token: String,
        _directory: TempDir,
    }

    #[test]
    fn plain_body_borrows_request_bytes_and_escaped_body_is_decoded_once() {
        let request = Bytes::from_static(br#"{"body":"  hello  "}"#);
        let body = post_body(&request).unwrap();
        assert_eq!(body, "hello");
        assert_eq!(body.as_ptr(), request[11..].as_ptr());
        let request = Bytes::from_static(br#"{"body":" \u00e9\n\"quoted\" ","ignored":[true,1]}"#);
        assert_eq!(post_body(&request).unwrap(), "é\n\"quoted\"");
        assert_eq!(
            post_body(&Bytes::from_static(br#"{"body":1}garbage"#)),
            Err("malformed JSON body")
        );
        assert_eq!(
            post_body(&Bytes::from_static(br#"{"body":1}"#)),
            Err("body is required")
        );
        assert_eq!(
            post_body(&Bytes::from_static(br#"["hello"]"#)),
            Err("body is required")
        );
        assert_eq!(
            post_body(&Bytes::from_static(br#"{"body":"\uD800"}"#)),
            Err("malformed JSON body")
        );
    }

    fn fixture() -> Fixture {
        let scratch = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("target/test-data");
        std::fs::create_dir_all(&scratch).unwrap();
        let directory = tempfile::Builder::new()
            .prefix("web-")
            .tempdir_in(scratch)
            .unwrap();
        let path = directory.path().join("feed.db");
        let connection = rusqlite::Connection::open(&path).unwrap();
        connection
            .execute_batch(include_str!("../../../schema.sql"))
            .unwrap();
        connection.execute_batch("INSERT INTO users (id, username) VALUES (1, 'stored-name'), (2, 'other-user'); INSERT INTO posts (id,user_id,body,created_at) VALUES (1,1,'seed post','2000-01-01T00:00:00.000Z');").unwrap();
        drop(connection);
        let db = Db::open(&path).unwrap();
        let token = encode(&Header::new(Algorithm::HS256), &json!({
            "sub":"1", "username":"token-name", "exp":SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs() + 3600,
        }), &EncodingKey::from_secret(b"test-secret")).unwrap();
        Fixture {
            app: router(db, Auth::new(b"test-secret")),
            token,
            _directory: directory,
        }
    }

    async fn request(
        fixture: &Fixture,
        method: &str,
        path: &str,
        authenticated: bool,
        body: &str,
    ) -> (StatusCode, String) {
        let mut builder = Request::builder().method(method).uri(path);
        if authenticated {
            builder = builder.header("Authorization", format!("Bearer {}", fixture.token));
        }
        let response = fixture
            .app
            .clone()
            .oneshot(builder.body(Body::from(body.to_owned())).unwrap())
            .await
            .unwrap();
        assert_eq!(
            response.headers().get(CONTENT_TYPE).unwrap(),
            "application/json"
        );
        let status = response.status();
        let bytes = response.into_body().collect().await.unwrap().to_bytes();
        (status, String::from_utf8(bytes.to_vec()).unwrap())
    }

    #[tokio::test]
    async fn authentication_precedes_id_and_json_validation() {
        let fixture = fixture();
        assert_eq!(
            request(&fixture, "POST", "/posts/abc/like", false, "").await,
            (
                StatusCode::UNAUTHORIZED,
                r#"{"error":"missing bearer token"}"#.to_owned()
            )
        );
        assert_eq!(
            request(&fixture, "POST", "/posts/%FF/like", false, "").await,
            (
                StatusCode::UNAUTHORIZED,
                r#"{"error":"missing bearer token"}"#.to_owned()
            )
        );
        assert_eq!(
            request(&fixture, "POST", "/posts", false, "{bad json").await,
            (
                StatusCode::UNAUTHORIZED,
                r#"{"error":"missing bearer token"}"#.to_owned()
            )
        );
        assert_eq!(
            request(&fixture, "POST", "/posts/abc/like", true, "").await,
            (
                StatusCode::BAD_REQUEST,
                r#"{"error":"invalid post id"}"#.to_owned()
            )
        );
        assert_eq!(
            request(&fixture, "POST", "/posts/%FF/like", true, "").await,
            (
                StatusCode::BAD_REQUEST,
                r#"{"error":"invalid post id"}"#.to_owned()
            )
        );
        assert_eq!(
            request(&fixture, "POST", "/posts", true, "{bad json").await,
            (
                StatusCode::BAD_REQUEST,
                r#"{"error":"malformed JSON body"}"#.to_owned()
            )
        );
    }

    #[tokio::test]
    async fn unicode_is_trimmed_and_length_counts_characters() {
        let fixture = fixture();
        for value in [
            json!({}),
            json!({"body":123}),
            json!({"body":"\u{2003}\n\t"}),
            json!(null),
        ] {
            assert_eq!(
                request(&fixture, "POST", "/posts", true, &value.to_string()).await,
                (
                    StatusCode::BAD_REQUEST,
                    r#"{"error":"body is required"}"#.to_owned()
                )
            );
        }
        let body = "é".repeat(500);
        let (status, response) = request(
            &fixture,
            "POST",
            "/posts",
            true,
            &json!({"body":format!("\u{2003}{body}\u{2003}")}).to_string(),
        )
        .await;
        assert_eq!(status, StatusCode::CREATED);
        let post: Value = serde_json::from_str(&response).unwrap();
        assert_eq!(post["post"]["body"], body);
        assert_eq!(post["post"]["author"], "token-name");
        assert_eq!(
            request(
                &fixture,
                "POST",
                "/posts",
                true,
                &json!({"body":"é".repeat(501)}).to_string()
            )
            .await,
            (
                StatusCode::BAD_REQUEST,
                r#"{"error":"body must be at most 500 characters"}"#.to_owned()
            )
        );
    }

    #[tokio::test]
    async fn likes_are_idempotent_and_visible_on_the_next_read() {
        let fixture = fixture();
        let (status, response) =
            request(&fixture, "POST", "/posts", true, r#"{"body":"  hello  "}"#).await;
        assert_eq!(status, StatusCode::CREATED);
        let post: Value = serde_json::from_str(&response).unwrap();
        let id = post["post"]["id"].as_i64().unwrap();
        let created = post["post"]["created_at"].as_str().unwrap();
        assert_eq!(
            response,
            format!(
                r#"{{"post":{{"id":{id},"body":"hello","created_at":"{created}","author":"token-name","like_count":0}}}}"#
            )
        );
        assert_eq!(
            request(&fixture, "POST", &format!("/posts/{id}/like"), true, "").await,
            (
                StatusCode::CREATED,
                format!(r#"{{"liked":true,"already_liked":false,"post_id":{id}}}"#)
            )
        );
        assert_eq!(
            request(&fixture, "POST", &format!("/posts/{id}/like"), true, "").await,
            (
                StatusCode::OK,
                format!(r#"{{"liked":true,"already_liked":true,"post_id":{id}}}"#)
            )
        );
        let (status, read) = request(&fixture, "GET", &format!("/posts/{id}"), false, "").await;
        assert_eq!(status, StatusCode::OK);
        // Reads join the stored author's name; writes use the verified token name.
        assert_eq!(
            read,
            format!(
                r#"{{"post":{{"id":{id},"body":"hello","created_at":"{created}","author":"stored-name","like_count":1}}}}"#
            )
        );
        let (status, feed) = request(&fixture, "GET", "/feed", false, "").await;
        assert_eq!(status, StatusCode::OK);
        let feed: Value = serde_json::from_str(&feed).unwrap();
        assert_eq!(feed["posts"][0]["id"], id);
        assert_eq!(feed["posts"][0]["like_count"], 1);
    }

    #[tokio::test]
    async fn routes_have_exact_errors_and_health_checks_sqlite() {
        let fixture = fixture();
        for id in ["0", "-1", "abc", "1.5", "+1", "%FF"] {
            assert_eq!(
                request(&fixture, "GET", &format!("/posts/{id}"), false, "").await,
                (
                    StatusCode::BAD_REQUEST,
                    r#"{"error":"invalid post id"}"#.to_owned()
                )
            );
        }
        assert_eq!(
            request(&fixture, "GET", "/posts/99999", false, "").await,
            (
                StatusCode::NOT_FOUND,
                r#"{"error":"post not found"}"#.to_owned()
            )
        );
        assert_eq!(
            request(&fixture, "POST", "/posts/99999/like", true, "").await,
            (
                StatusCode::NOT_FOUND,
                r#"{"error":"post not found"}"#.to_owned()
            )
        );
        assert_eq!(
            request(&fixture, "GET", "/unknown", false, "").await,
            (StatusCode::NOT_FOUND, r#"{"error":"not found"}"#.to_owned())
        );
        let (status, response) = request(&fixture, "GET", "/health", false, "").await;
        assert_eq!(status, StatusCode::OK);
        let health: Value = serde_json::from_str(&response).unwrap();
        assert_eq!(
            response,
            format!(
                r#"{{"status":"ok","db":"ok","uptime_s":{}}}"#,
                health["uptime_s"]
            )
        );
    }
}
