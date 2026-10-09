mod tests {
    use super::*;
    use jsonwebtoken::{EncodingKey, Header, encode};
    use serde_json::json;

    fn authenticate(
        payload: Value,
        algorithm: Algorithm,
        secret: &[u8],
    ) -> Result<User, &'static str> {
        let token = encode(
            &Header::new(algorithm),
            &payload,
            &EncodingKey::from_secret(secret),
        )
        .unwrap();
        let mut headers = HeaderMap::new();
        headers.insert(AUTHORIZATION, format!("Bearer {token}").parse().unwrap());
        Auth::new(b"test-secret").authenticate(&headers)
    }

    fn claims() -> Value {
        json!({"sub":"1", "username":"token-name", "exp":SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs() + 3600})
    }

    #[test]
    fn verifies_every_signature_and_only_accepts_hs256() {
        assert_eq!(
            authenticate(claims(), Algorithm::HS256, b"test-secret").unwrap(),
            User {
                user_id: 1,
                username: "token-name".to_owned()
            }
        );
        assert_eq!(
            authenticate(claims(), Algorithm::HS256, b"wrong-secret"),
            Err("invalid or expired token")
        );
        assert_eq!(
            authenticate(claims(), Algorithm::HS384, b"test-secret"),
            Err("invalid or expired token")
        );
        let auth = Auth::new(b"test-secret");
        let mut headers = HeaderMap::new();
        assert_eq!(auth.authenticate(&headers), Err("missing bearer token"));
        headers.insert(AUTHORIZATION, "bearer abc".parse().unwrap());
        assert_eq!(auth.authenticate(&headers), Err("missing bearer token"));
        headers.insert(AUTHORIZATION, "Bearer abc".parse().unwrap());
        assert_eq!(auth.authenticate(&headers), Err("invalid or expired token"));
        headers.insert(
            AUTHORIZATION,
            axum::http::HeaderValue::from_bytes("Bearer é".as_bytes()).unwrap(),
        );
        assert_eq!(auth.authenticate(&headers), Err("invalid or expired token"));
    }

    #[test]
    fn expiration_is_required_and_has_no_leeway() {
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs();
        for expiration in [
            json!(now),
            json!(now - 1),
            json!("9999999999"),
            json!(null),
            json!(-1),
        ] {
            let mut payload = claims();
            payload["exp"] = expiration;
            assert_eq!(
                authenticate(payload, Algorithm::HS256, b"test-secret"),
                Err("invalid or expired token")
            );
        }
        let mut payload = claims();
        payload.as_object_mut().unwrap().remove("exp");
        assert_eq!(
            authenticate(payload, Algorithm::HS256, b"test-secret"),
            Err("invalid or expired token")
        );
    }

    #[test]
    fn valid_signatures_with_invalid_user_claims_have_payload_errors() {
        for subject in [
            json!(1),
            json!("0"),
            json!("-1"),
            json!("+1"),
            json!("1.5"),
            json!("abc"),
            json!(null),
        ] {
            let mut payload = claims();
            payload["sub"] = subject;
            assert_eq!(
                authenticate(payload, Algorithm::HS256, b"test-secret"),
                Err("invalid token payload")
            );
        }
        for username in [json!(1), json!(null)] {
            let mut payload = claims();
            payload["username"] = username;
            assert_eq!(
                authenticate(payload, Algorithm::HS256, b"test-secret"),
                Err("invalid token payload")
            );
        }
        let mut payload = claims();
        payload.as_object_mut().unwrap().remove("username");
        assert_eq!(
            authenticate(payload, Algorithm::HS256, b"test-secret"),
            Err("invalid token payload")
        );
    }
}
