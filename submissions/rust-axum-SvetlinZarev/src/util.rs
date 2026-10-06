pub fn parse_id(raw: &str) -> Option<i64> {
    if raw.is_empty() || !raw.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    match raw.parse::<i64>() {
        Ok(n) if n > 0 => Some(n),
        _ => None,
    }
}
