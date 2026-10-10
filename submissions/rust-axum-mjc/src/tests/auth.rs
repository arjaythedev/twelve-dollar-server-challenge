mod tests {
    use super::*;
    use jsonwebtoken::{EncodingKey, Header, encode};
    use serde::ser::SerializeMap;
    use serde_json::json;

    struct DuplicateClaim(&'static str);
    impl serde::Serialize for DuplicateClaim {
        fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
            let exp = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_secs()
                + 3600;
            let mut map = serializer.serialize_map(Some(4))?;
            map.serialize_entry("exp", &exp)?;
            map.serialize_entry("sub", "1")?;
            map.serialize_entry("username", "first")?;
            match self.0 {
                "username" => map.serialize_entry("username", "last")?,
                "exp" => map.serialize_entry("exp", &0u64)?,
                "sub" => map.serialize_entry("sub", &2u64)?,
                _ => unreachable!(),
            }
            map.end()
        }
    }

    fn authenticate_duplicate(key: &'static str) -> Result<User, &'static str> {
        let token = encode(
            &Header::new(Algorithm::HS256),
            &DuplicateClaim(key),
            &EncodingKey::from_secret(b"test-secret"),
        )
        .unwrap();
        let mut headers = HeaderMap::new();
        headers.insert(AUTHORIZATION, format!("Bearer {token}").parse().unwrap());
        Auth::new(b"test-secret").authenticate(&headers)
    }

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

    fn authenticate_id(
        payload: Value,
        algorithm: Algorithm,
        secret: &[u8],
    ) -> Result<i64, &'static str> {
        let token = encode(
            &Header::new(algorithm),
            &payload,
            &EncodingKey::from_secret(secret),
        )
        .unwrap();
        let mut headers = HeaderMap::new();
        headers.insert(AUTHORIZATION, format!("Bearer {token}").parse().unwrap());
        Auth::new(b"test-secret").authenticate_id(&headers)
    }

    fn authenticate_raw(payload: &str) -> Result<User, &'static str> {
        authenticate_token(&token_with_payload(payload.as_bytes()))
    }

    fn authenticate_token(token: &str) -> Result<User, &'static str> {
        let mut headers = HeaderMap::new();
        headers.insert(AUTHORIZATION, format!("Bearer {token}").parse().unwrap());
        Auth::new(b"test-secret").authenticate(&headers)
    }

    fn token_with_payload(payload: &[u8]) -> String {
        const BASE64URL: &[u8; 64] =
            b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        let mut encoded_payload = String::with_capacity(payload.len().div_ceil(3) * 4);
        for chunk in payload.chunks(3) {
            let a = chunk[0];
            let b = *chunk.get(1).unwrap_or(&0);
            let c = *chunk.get(2).unwrap_or(&0);
            encoded_payload.push(BASE64URL[(a >> 2) as usize] as char);
            encoded_payload.push(BASE64URL[(((a & 0b11) << 4) | (b >> 4)) as usize] as char);
            if chunk.len() > 1 {
                encoded_payload.push(BASE64URL[(((b & 0b1111) << 2) | (c >> 6)) as usize] as char);
            }
            if chunk.len() > 2 {
                encoded_payload.push(BASE64URL[(c & 0b11_1111) as usize] as char);
            }
        }

        let key = EncodingKey::from_secret(b"test-secret");
        let header = encode(&Header::new(Algorithm::HS256), &json!({}), &key)
            .unwrap()
            .split('.')
            .next()
            .unwrap()
            .to_owned();
        let signing_input = format!("{header}.{encoded_payload}");
        let signature =
            jsonwebtoken::crypto::sign(signing_input.as_bytes(), &key, Algorithm::HS256).unwrap();
        format!("{signing_input}.{signature}")
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
            let result = authenticate(payload, Algorithm::HS256, b"test-secret");
            assert_eq!(result, Err("invalid or expired token"));
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
            json!({"id":"1"}),
            json!(["1"]),
            json!(1),
            json!("0"),
            json!("-1"),
            json!("+1"),
            json!("1.5"),
            json!("abc"),
            json!(null),
        ] {
            let mut payload = claims();
            let subject_label = subject.to_string();
            payload["sub"] = subject;
            assert_eq!(
                authenticate(payload, Algorithm::HS256, b"test-secret"),
                Err("invalid token payload"),
                "subject={subject_label}"
            );
        }
        let mut expired_with_structured_subject = claims();
        expired_with_structured_subject["sub"] = json!({"id":"1"});
        expired_with_structured_subject["exp"] = json!(0);
        assert_eq!(
            authenticate(
                expired_with_structured_subject.clone(),
                Algorithm::HS256,
                b"test-secret"
            ),
            Err("invalid or expired token")
        );
        let mut invalid_signature_subject = claims();
        invalid_signature_subject["sub"] = json!({"id":"1"});
        assert_eq!(
            authenticate(invalid_signature_subject, Algorithm::HS256, b"wrong-secret"),
            Err("invalid or expired token")
        );
        let mut payload = claims();
        payload.as_object_mut().unwrap().remove("sub");
        assert_eq!(
            authenticate(payload, Algorithm::HS256, b"test-secret"),
            Err("invalid token payload")
        );
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

    #[test]
    fn authenticate_id_matches_user_authentication_without_retaining_username() {
        let mut escaped = claims();
        escaped["username"] = json!("line\nbreak-\"quoted\"");
        let mut invalid_exp = claims();
        invalid_exp["exp"] = json!("tomorrow");
        let mut invalid_sub = claims();
        invalid_sub["sub"] = json!("+1");
        let mut invalid_username = claims();
        invalid_username["username"] = json!(false);
        let mut structured_sub = claims();
        structured_sub["sub"] = json!({"id": "1"});

        for payload in [
            claims(),
            escaped,
            invalid_exp,
            invalid_sub,
            invalid_username,
            structured_sub.clone(),
        ] {
            let user_result = authenticate(payload.clone(), Algorithm::HS256, b"test-secret")
                .map(|user| user.user_id);
            assert_eq!(
                authenticate_id(payload, Algorithm::HS256, b"test-secret"),
                user_result
            );
        }
        assert_eq!(
            authenticate_id(structured_sub, Algorithm::HS256, b"wrong-secret"),
            Err("invalid or expired token")
        );
    }

    #[test]
    fn typed_decode_fallback_preserves_duplicate_claim_value_semantics() {
        assert_eq!(
            authenticate_duplicate("username"),
            Ok(User {
                user_id: 1,
                username: "last".to_owned()
            })
        );
        assert_eq!(
            authenticate_duplicate("exp"),
            Err("invalid or expired token")
        );
        assert_eq!(authenticate_duplicate("sub"), Err("invalid token payload"));
    }

    #[test]
    fn rejects_invalid_unicode_in_ignored_claim_values_and_keys() {
        let exp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs()
            + 3600;
        for payload in [
            format!(r#"{{"exp":{exp},"sub":"1","username":"user","ignored":"\ud800"}}"#),
            format!(
                r#"{{"exp":{exp},"sub":"1","username":"user","ignored":{{"nested":["\ud800"]}}}}"#
            ),
            format!(
                r#"{{"exp":{exp},"sub":"1","username":"user","ignored":{{"\ud800":"value"}}}}"#
            ),
            format!(r#"{{"exp":{exp},"sub":"1","username":["\ud800"]}}"#),
        ] {
            assert_eq!(authenticate_raw(&payload), Err("invalid or expired token"));
        }
    }

    #[test]
    fn rejects_invalid_utf8_bytes_in_claims_for_both_auth_paths() {
        let exp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs()
            + 3600;
        let common = format!(r#"{{"exp":{exp},"sub":"1","username":"user","#);
        let cases = [
            (format!(r#"{common}"ignored":""#), "\"}".to_owned()),
            (
                format!(r#"{common}"ignored":{{"nested":[""#),
                "\"]}}".to_owned(),
            ),
            (
                format!(r#"{common}"ignored":{{""#),
                "\":\"value\"}}".to_owned(),
            ),
            (
                format!(r#"{{"exp":{exp},"sub":"1","username":[""#),
                "\"]}".to_owned(),
            ),
            (
                "{\"exp\":{\"nested\":\"".to_owned(),
                r#""},"sub":"1","username":"user"}"#.to_owned(),
            ),
            (
                format!(r#"{{"exp":{exp},"sub":{{"nested":""#),
                r#""},"username":"user"}"#.to_owned(),
            ),
            (
                format!(r#"{{"exp":{exp},"sub":"1","username":"user",""#),
                r#"":"value"}"#.to_owned(),
            ),
        ];

        for invalid_utf8 in [&[0xff][..], &[0xc3, 0x28][..]] {
            let payloads = cases.iter().map(|(prefix, suffix)| {
                let mut valid_payload = prefix.as_bytes().to_vec();
                valid_payload.push(b'x');
                valid_payload.extend_from_slice(suffix.as_bytes());
                serde_json::from_slice::<Value>(&valid_payload).unwrap();

                let mut payload = prefix.as_bytes().to_vec();
                payload.extend_from_slice(invalid_utf8);
                payload.extend_from_slice(suffix.as_bytes());
                payload
            });

            for payload in payloads {
                let token = token_with_payload(&payload);
                let mut headers = HeaderMap::new();
                headers.insert(AUTHORIZATION, format!("Bearer {token}").parse().unwrap());
                let auth = Auth::new(b"test-secret");
                assert_eq!(auth.authenticate(&headers), Err("invalid or expired token"));
                assert_eq!(
                    auth.authenticate_id(&headers),
                    Err("invalid or expired token")
                );
            }
        }
    }
}
