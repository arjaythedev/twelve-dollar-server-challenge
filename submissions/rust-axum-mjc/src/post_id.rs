/// Parse a positive decimal post id from one raw URI path segment.
///
/// Percent escapes are decoded one byte at a time, without allocating. The
/// decoded value must consist only of ASCII decimal digits.
pub(crate) fn parse_post_id(raw_segment: &str) -> Option<i64> {
    let bytes = raw_segment.as_bytes();
    let mut index = 0;
    let mut value = 0_i64;
    let mut found_digit = false;

    while index < bytes.len() {
        let byte = if bytes[index] == b'%' {
            let high = hex(*bytes.get(index + 1)?)?;
            let low = hex(*bytes.get(index + 2)?)?;
            index += 3;
            (high << 4) | low
        } else {
            let byte = bytes[index];
            index += 1;
            byte
        };

        if !byte.is_ascii_digit() {
            return None;
        }
        found_digit = true;
        value = value.checked_mul(10)?.checked_add(i64::from(byte - b'0'))?;
    }

    (found_digit && value > 0).then_some(value)
}

fn hex(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

#[cfg(test)]
include!("tests/post_id.rs");
