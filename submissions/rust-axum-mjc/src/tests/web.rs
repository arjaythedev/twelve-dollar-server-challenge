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
        io::{BufRead, BufReader, Read, Write},
        net::TcpStream,
        path::PathBuf,
        time::Duration,
        time::{SystemTime, UNIX_EPOCH},
    };
    use tempfile::TempDir;
    use tower::{ServiceExt, util::BoxCloneService};

    struct Fixture {
        app: BoxCloneService<axum::extract::Request, Response, Infallible>,
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
            post_body(&Bytes::from_static(br#"{"body":1,"ignored": }"#)),
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

    #[test]
    fn successful_json_response_reuses_cleared_request_header_storage() {
        let mut headers = HeaderMap::with_capacity(16);
        for index in 0..8 {
            headers.insert(
                format!("x-request-{index}")
                    .parse::<axum::http::HeaderName>()
                    .unwrap(),
                HeaderValue::from_static("private"),
            );
        }
        let header_capacity = headers.capacity();
        let mut body = Vec::with_capacity(64);
        body.extend_from_slice(b"{}");
        let pointer = body.as_ptr();
        let response = encoded_json(&mut headers, StatusCode::OK, body);

        assert_eq!(response.headers().capacity(), header_capacity);
        assert_eq!(response.headers()[CONTENT_TYPE], "application/json");
        assert!(!response.headers().contains_key("x-request-0"));
        let body = response.into_body().into_inner().unwrap();
        assert_eq!(body.as_ptr(), pointer);
        assert_eq!(body.as_ref(), b"{}");
    }

    #[tokio::test]
    async fn response_headers_do_not_reflect_request_headers() {
        let fixture = fixture();
        let response = fixture
            .app
            .clone()
            .oneshot(
                Request::builder()
                    .method("GET")
                    .uri("/posts/1")
                    .header("authorization", "Bearer secret")
                    .header("x-private-request", "private")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();

        assert_eq!(response.status(), StatusCode::OK);
        assert_eq!(response.headers()[CONTENT_TYPE], "application/json");
        assert!(!response.headers().contains_key("authorization"));
        assert!(!response.headers().contains_key("x-private-request"));
    }

    fn fixture() -> Fixture {
        let scratch = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("target/test-data");
        std::fs::create_dir_all(&scratch).unwrap();
        let directory = tempfile::Builder::new()
            .prefix("web-")
            .tempdir_in(scratch)
            .unwrap();
        let db = seeded_db(&directory, true);
        let token = encode(&Header::new(Algorithm::HS256), &json!({
            "sub":"1", "username":"token-name", "exp":SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs() + 3600,
        }), &EncodingKey::from_secret(b"test-secret")).unwrap();
        Fixture {
            app: BoxCloneService::new(router(db, Auth::new(b"test-secret"))),
            token,
            _directory: directory,
        }
    }

    fn seeded_db(directory: &TempDir, read_batching: bool) -> Db {
        let path = directory.path().join("feed.db");
        let connection = rusqlite::Connection::open(&path).unwrap();
        connection
            .execute_batch(include_str!("../../../../schema.sql"))
            .unwrap();
        connection.execute_batch("INSERT INTO users (id, username) VALUES (1, 'stored-name'), (2, 'other-user'); INSERT INTO posts (id,user_id,body,created_at) VALUES (1,1,'seed post','2000-01-01T00:00:00.000Z');").unwrap();
        drop(connection);
        Db::open_with_read_batching(&path, read_batching).unwrap()
    }

    fn read_http_response(
        stream: &mut BufReader<TcpStream>,
        body_allowed: bool,
    ) -> (u16, std::collections::HashMap<String, String>, Vec<u8>) {
        let mut line = String::new();
        stream.read_line(&mut line).unwrap();
        let status = line.split_whitespace().nth(1).unwrap().parse().unwrap();
        let mut headers = std::collections::HashMap::new();
        loop {
            line.clear();
            stream.read_line(&mut line).unwrap();
            if line == "\r\n" {
                break;
            }
            let (name, value) = line.split_once(':').unwrap();
            headers.insert(name.to_ascii_lowercase(), value.trim().to_owned());
        }
        let mut body = vec![
            0;
            if body_allowed {
                headers["content-length"].parse().unwrap()
            } else {
                0
            }
        ];
        stream.read_exact(&mut body).unwrap();
        (status, headers, body)
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

    #[tokio::test]
    async fn dispatch_preserves_method_allow_and_encoded_id_behavior() {
        let fixture = fixture();
        let response = fixture
            .app
            .clone()
            .oneshot(
                Request::builder()
                    .method("POST")
                    .uri("/feed")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::METHOD_NOT_ALLOWED);
        assert_eq!(response.headers().get(ALLOW).unwrap(), "GET,HEAD");

        let response = fixture
            .app
            .clone()
            .oneshot(
                Request::builder()
                    .method("GET")
                    .uri("/posts/1/like")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::METHOD_NOT_ALLOWED);
        assert_eq!(response.headers().get(ALLOW).unwrap(), "POST");

        let response = fixture
            .app
            .clone()
            .oneshot(
                Request::builder()
                    .method("PATCH")
                    .uri("/unknown")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::NOT_FOUND);

        let (status, body) = request(&fixture, "GET", "/posts/%31", false, "").await;
        assert_eq!(status, StatusCode::OK);
        assert!(body.contains("seed post"));
        assert_eq!(
            request(&fixture, "GET", "/posts/%2F", false, "").await,
            (
                StatusCode::BAD_REQUEST,
                r#"{"error":"invalid post id"}"#.to_owned()
            )
        );
        assert_eq!(
            request(&fixture, "GET", "/posts/1/extra", false, "").await,
            (StatusCode::NOT_FOUND, r#"{"error":"not found"}"#.to_owned())
        );
        assert_eq!(
            request(&fixture, "GET", "/posts/1/", false, "").await,
            (StatusCode::NOT_FOUND, r#"{"error":"not found"}"#.to_owned())
        );
        let get_response = fixture
            .app
            .clone()
            .oneshot(
                Request::builder()
                    .method("GET")
                    .uri("/posts/1")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert!(!get_response.headers().contains_key(CONTENT_LENGTH));
        let get_length = get_response.body().size_hint().exact().unwrap();
        let head_response = fixture
            .app
            .clone()
            .oneshot(
                Request::builder()
                    .method("HEAD")
                    .uri("/posts/1")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(head_response.status(), StatusCode::OK);
        assert_eq!(
            head_response.headers()[CONTENT_LENGTH],
            get_length.to_string()
        );
        assert!(
            head_response
                .into_body()
                .collect()
                .await
                .unwrap()
                .to_bytes()
                .is_empty()
        );

        let get_response = fixture
            .app
            .clone()
            .oneshot(
                Request::builder()
                    .method("GET")
                    .uri("/unknown")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert!(!get_response.headers().contains_key(CONTENT_LENGTH));
        let get_length = get_response.body().size_hint().exact().unwrap();
        let head_response = fixture
            .app
            .clone()
            .oneshot(
                Request::builder()
                    .method("HEAD")
                    .uri("/unknown")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(head_response.status(), StatusCode::NOT_FOUND);
        assert_eq!(
            head_response.headers()[CONTENT_LENGTH],
            get_length.to_string()
        );
        assert!(
            head_response
                .into_body()
                .collect()
                .await
                .unwrap()
                .to_bytes()
                .is_empty()
        );
    }

    #[tokio::test]
    async fn http1_frames_responses_and_reuses_connection_after_head() {
        for read_batching in [false, true] {
            exercise_http1_connection(read_batching).await;
        }
    }

    async fn exercise_http1_connection(read_batching: bool) {
        let scratch = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("target/test-data");
        std::fs::create_dir_all(&scratch).unwrap();
        let directory = tempfile::Builder::new()
            .prefix("web-wire-")
            .tempdir_in(scratch)
            .unwrap();
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        listener.set_nonblocking(true).unwrap();
        let address = listener.local_addr().unwrap();
        let service = hyper_router(
            seeded_db(&directory, read_batching),
            Auth::new(b"test-secret"),
        );
        let (shutdown_tx, shutdown_rx) = tokio::sync::oneshot::channel();
        let server = std::thread::spawn(move || {
            tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .unwrap()
                .block_on(async {
                    let listener = tokio::net::TcpListener::from_std(listener).unwrap();
                    crate::server::serve(listener, service, async {
                        let _ = shutdown_rx.await;
                    })
                    .await
                })
        });

        let responses = tokio::task::spawn_blocking(move || {
            let stream = TcpStream::connect(address).unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(5)))
                .unwrap();
            let mut stream = BufReader::new(stream);

            stream
                .get_mut()
                .write_all(b"GET /posts/1 HTTP/1.1\r\nHost: localhost\r\n\r\n")
                .unwrap();
            let post = read_http_response(&mut stream, true);

            stream
                .get_mut()
                .write_all(b"HEAD /unknown HTTP/1.1\r\nHost: localhost\r\n\r\n")
                .unwrap();
            let missing_head = read_http_response(&mut stream, false);

            stream
                .get_mut()
                .write_all(b"GET /unknown HTTP/1.1\r\nHost: localhost\r\n\r\n")
                .unwrap();
            let missing_get = read_http_response(&mut stream, true);
            (post, missing_head, missing_get)
        })
        .await
        .unwrap();

        let (post_response, missing_head, missing_get) = responses;
        assert_eq!(post_response.0, 200);
        assert_eq!(
            post_response.1["content-length"].parse::<usize>().unwrap(),
            post_response.2.len()
        );
        assert!(
            std::str::from_utf8(&post_response.2)
                .unwrap()
                .contains("seed post")
        );

        assert_eq!(missing_head.0, 404);
        assert_eq!(
            missing_head.1["content-length"],
            missing_get.1["content-length"]
        );
        assert_eq!(missing_head.1["content-length"], "21");

        assert_eq!(missing_get.0, 404);
        assert_eq!(
            missing_get.1["content-length"].parse::<usize>().unwrap(),
            missing_get.2.len()
        );
        assert_eq!(missing_get.2, br#"{"error":"not found"}"#);

        shutdown_tx.send(()).unwrap();
        server.join().unwrap().unwrap();
    }

    #[tokio::test]
    async fn dispatch_keeps_create_body_limit() {
        let fixture = fixture();
        for authenticated in [false, true] {
            let mut builder = Request::builder().method("POST").uri("/posts");
            if authenticated {
                builder = builder.header("Authorization", format!("Bearer {}", fixture.token));
            }
            let request = builder
                .body(Body::from(vec![b' '; 2 * 1024 * 1024 + 1]))
                .unwrap();
            let response = fixture.app.clone().oneshot(request).await.unwrap();
            assert_eq!(response.status(), StatusCode::PAYLOAD_TOO_LARGE);
            assert!(response.headers().contains_key(CONTENT_TYPE));
            assert!(!response.headers().contains_key("authorization"));
            assert!(
                !response
                    .into_body()
                    .collect()
                    .await
                    .unwrap()
                    .to_bytes()
                    .is_empty()
            );
        }
    }
}
