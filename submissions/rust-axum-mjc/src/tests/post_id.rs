mod tests {
    use super::parse_post_id;

    #[test]
    fn parses_digits_after_a_single_percent_decode() {
        assert_eq!(parse_post_id("%31%32"), Some(12));
        assert_eq!(parse_post_id("00012"), Some(12));
        assert_eq!(parse_post_id("9223372036854775807"), Some(i64::MAX));
    }

    #[test]
    fn rejects_non_digits_malformed_escapes_and_overflow() {
        for value in [
            "",
            "0",
            "-1",
            "+1",
            " 1",
            "1 ",
            "%2F",
            "%",
            "%G1",
            "%2531",
            "%C3%A9",
            "9223372036854775808",
        ] {
            assert_eq!(parse_post_id(value), None, "{value}");
        }
    }
}
