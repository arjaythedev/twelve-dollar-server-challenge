mod tests {
    use super::Post;

    #[test]
    fn fixed_key_writer_matches_derived_json_bytes() {
        let posts = [
            Post {
                id: i64::MIN,
                body: "quotes: \" slash: \\ controls: \0\n\r\t\u{0001} unicode: 雪🙂",
                created_at: "2026-10-10T19:45:12.345Z",
                author: "a\"uthor\\\n雪",
                like_count: i64::MAX,
            },
            Post {
                id: i64::MAX,
                body: "",
                created_at: "",
                author: "",
                like_count: i64::MIN,
            },
        ];

        for post in posts {
            let expected = serde_json::to_vec(&post).unwrap();
            let mut actual = Vec::new();
            post.write_json(&mut actual).unwrap();
            assert_eq!(actual, expected);
        }
    }
}
