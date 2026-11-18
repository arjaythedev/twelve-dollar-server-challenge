// MIT. Codec routines adapted from goose-iouring-ahmadsh2007 and sstadick PR 1.
import std;
import simd_bytes;
namespace json;

fn push_int(out: u8[>..]&, v: i64) {
    if v >= 0 && v < 10 { out.push(('0' + v) as! u8); return; }
    var d: u8[20] = [0; 20];
    var n = 0;
    var x = v;
    if v < 0 { out.push('-'); x = -v; }
    while x > 0 {
        d[19 - n] = ('0' + x % 10) as! u8;
        x = x / 10;
        n++;
    }
    out.append(d[20 - n..]);
}

// The upper three bits distinguish controls from all other bytes, including UTF-8.
// Goose's lane loop becomes a vector operation in native builds.
inline fn json_mask<V>(s: const u8[:]) -> u32 {
    let v = simd_bytes::load<V>(s);
    var upper = v;
    for &c in upper.lanes { c &= 0xe0; }
    return upper.eq(0).bit_or(v.eq('"')).bit_or(v.eq('\\')).to_bits();
}

// The length of s's prefix that needs no escaping.
inline fn plain_prefix(s: const u8[:]) -> i64 {
    if s.len == 0 { return 0; }
    if s[0] < 32 || s[0] == '"' || s[0] == '\\' { return 0; }
    if s.len >= 32 {
        var i = 0;
        while i <= s.len - 32 {
            let mask = json_mask<simd_bytes::U8x32>(s[i..]);
            if mask != 0 { return i + ctz(mask); }
            i += 32;
        }
        // An overlapping final load stays inside the slice; its prefix was checked above.
        if i < s.len {
            let start = s.len - 32;
            let mask = json_mask<simd_bytes::U8x32>(s[start..]);
            if mask != 0 { return start + ctz(mask); }
        }
    } else if s.len >= 16 {
        let first = json_mask<simd_bytes::U8x16>(s);
        if first != 0 { return ctz(first); }
        if s.len > 16 {
            let start = s.len - 16;
            let last = json_mask<simd_bytes::U8x16>(s[start..]);
            if last != 0 { return start + ctz(last); }
        }
    } else {
        for c, i in s { if c < 32 || c == '"' || c == '\\' { return i; } }
    }
    return s.len;
}

let HEX = "0123456789abcdef";

fn esc(out: u8[>..]&, s: const u8[:]) {
    var i = 0;
    while i < s.len {
        let k = plain_prefix(s[i..]);
        if k > 0 { out.append(s[i..i + k]); i += k; }
        if i >= s.len { break; }
        let c = s[i];
        if c == '"' { out.append("\\\""); }
        else if c == '\\' { out.append("\\\\"); }
        else if c == '\n' { out.append("\\n"); }
        else if c == '\r' { out.append("\\r"); }
        else if c == '\t' { out.append("\\t"); }
        else if c == 8 { out.append("\\b"); }
        else if c == 12 { out.append("\\f"); }
        else {
            out.append("\\u00");
            out.push(HEX[(c >> 4) as i64]);
            out.push(HEX[(c & 15) as i64]);
        }
        i++;
    }
}

fn js_space(cp: i64) -> bool {
    if cp < 0x80 { return (cp >= 9 && cp <= 13) || cp == 32; }
    return cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 ||
        cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000 || cp == 0xfeff;
}

// One code point of well-formed UTF-8 at s[i]: (code point, bytes), or (0, 0) if malformed.
fn utf8_at(s: const u8[:], i: i64) -> i64, i64 {
    let b = s[i];
    if b < 0x80 { return b, 1; }
    var n = 0;
    var cp = 0;
    if b >= 0xc2 && b <= 0xdf { n = 2; cp = (b & 0x1f); }
    else if b >= 0xe0 && b <= 0xef { n = 3; cp = (b & 0x0f); }
    else if b >= 0xf0 && b <= 0xf4 { n = 4; cp = (b & 0x07); }
    else { return 0, 0; }
    if i + n > s.len { return 0, 0; }
    for k in 1..n {
        let x = s[i + k];
        if (x & 0xc0) != 0x80 { return 0, 0; }
        cp = (cp << 6) | (x & 0x3f);
    }
    if (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff {
        return 0, 0;
    }
    return cp, n;
}

// The body trimmed (byte range) and its length in characters (code points, the unit of the
// schema's CHECK); -1 characters if it is not well-formed UTF-8.
fn trim_count(s: const u8[:]) -> i64, i64, i64 {
    var ascii = true;
    for c in s { if c >= 0x80 { ascii = false; break; } }
    if ascii {
        var i = 0;
        var j = s.len;
        while i < j && js_space(s[i]) { i++; }
        while j > i && js_space(s[j - 1]) { j--; }
        return i, j, j - i;
    }
    var start = -1;
    var end = 0;
    var count = 0;
    var kept = 0;
    var i = 0;
    while i < s.len {
        let cp, n = utf8_at(s, i);
        if n == 0 { return 0, 0, -1; }
        if !js_space(cp) {
            if start < 0 { start = i; }
            count++;
            kept = count;
            end = i + n;
        } else if start >= 0 {
            count++;
        }
        i += n;
    }
    if start < 0 { return 0, 0, 0; }
    return start, end, kept;
}

// ---------------------------------------------------------------------------------------------
// JSON input: a validating reader over a whole document.

fn ws(s: const u8[:], p: i64&) {
    while p < s.len {
        let c = s[p];
        if c != ' ' && c != '\t' && c != '\n' && c != '\r' { return; }
        p++;
    }
}

fn hexval(c: u8) -> i64 {
    if c >= '0' && c <= '9' { return c - '0'; }
    if c >= 'a' && c <= 'f' { return (c - 'a') as i64 + 10; }
    if c >= 'A' && c <= 'F' { return (c - 'A') as i64 + 10; }
    return -1;
}

fn hex4(s: const u8[:], i: i64) -> i64 {
    if i + 4 > s.len { return -1; }
    var v = 0;
    for k in 4 {
        let h = hexval(s[i + k]);
        if h < 0 { return -1; }
        v = v * 16 + h;
    }
    return v;
}

fn valid_utf8(s: const u8[:]) -> bool {
    var i = 0;
    while i < s.len {
        if s[i] < 0x80 { i++; continue; }
        let cp, n = utf8_at(s, i);
        if n == 0 { return false; }
        i += n;
    }
    return true;
}

// The string at s[p] (which is '"'): its raw contents s[start..end], whether it has escapes,
// and whether it is well formed (escapes, no raw control characters, UTF-8). p moves past it.
fn read_string(s: const u8[:], p: i64&) -> i64, i64, bool, bool {
    let start = p + 1;
    var i = start;
    var escd = false;
    var high = false;
    loop {
        i += plain_prefix(s[i..]);
        if i >= s.len { return 0, 0, false, false; }
        let c = s[i];
        if c == '"' { break; }
        if c != '\\' { return 0, 0, false, false; } // a raw control character
        escd = true;
        if i + 1 >= s.len { return 0, 0, false, false; }
        let e = s[i + 1];
        if e == 'u' {
            if hex4(s, i + 2) < 0 { return 0, 0, false, false; }
            i += 6;
        } else if e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't' {
            i += 2;
        } else {
            return 0, 0, false, false;
        }
    }
    let raw = s[start..i];
    for c in raw { if c >= 0x80 { high = true; break; } }
    if high && !valid_utf8(raw) { return 0, 0, false, false; }
    p = i + 1;
    return start, i, escd, true;
}

fn put_utf8(out: u8[>..]&, cp: i64) {
    if cp < 0x80 { out.push(cp as! u8); }
    else if cp < 0x800 { out.push((0xc0 | (cp >> 6)) as! u8); out.push((0x80 | (cp & 0x3f)) as! u8); }
    else if cp < 0x10000 {
        out.push((0xe0 | (cp >> 12)) as! u8);
        out.push((0x80 | ((cp >> 6) & 0x3f)) as! u8);
        out.push((0x80 | (cp & 0x3f)) as! u8);
    } else {
        out.push((0xf0 | (cp >> 18)) as! u8);
        out.push((0x80 | ((cp >> 12) & 0x3f)) as! u8);
        out.push((0x80 | ((cp >> 6) & 0x3f)) as! u8);
        out.push((0x80 | (cp & 0x3f)) as! u8);
    }
}

// Decodes the escapes of a well-formed string's raw contents (read_string checked them).
// A surrogate pair becomes one code point; a lone surrogate becomes U+FFFD.
fn unescape(raw: const u8[:], out: u8[>..]&) {
    var i = 0;
    while i < raw.len {
        let c = raw[i];
        if c != '\\' { out.push(c); i++; continue; }
        let e = raw[i + 1];
        if e == 'u' {
            var cp = hex4(raw, i + 2);
            i += 6;
            if cp >= 0xd800 && cp <= 0xdbff && i + 6 <= raw.len && raw[i] == '\\' && raw[i + 1] == 'u' {
                let lo = hex4(raw, i + 2);
                if lo >= 0xdc00 && lo <= 0xdfff {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    i += 6;
                }
            }
            if cp >= 0xd800 && cp <= 0xdfff { cp = 0xfffd; }
            put_utf8(out, cp);
            continue;
        }
        let d: u8 = match e { 'b' => 8, 'f' => 12, 'n' => '\n', 'r' => '\r', 't' => '\t', _ => e };
        out.push(d);
        i += 2;
    }
}

fn lit(s: const u8[:], p: i64&, word: const u8[:]) -> bool {
    if p + word.len > s.len || s[p..p + word.len] != word { return false; }
    p += word.len;
    return true;
}

// A JSON number at s[p]: its value, and whether it is well formed. p moves past it.
fn read_number(s: const u8[:], p: i64&) -> f64, bool {
    let start = p;
    var i = p;
    if i < s.len && s[i] == '-' { i++; }
    if i >= s.len { return 0.0, false; }
    if s[i] == '0' { i++; }
    else if s[i] >= '1' && s[i] <= '9' { while i < s.len && s[i] >= '0' && s[i] <= '9' { i++; } }
    else { return 0.0, false; }
    if i < s.len && s[i] == '.' {
        i++;
        let d = i;
        while i < s.len && s[i] >= '0' && s[i] <= '9' { i++; }
        if i == d { return 0.0, false; }
    }
    if i < s.len && (s[i] == 'e' || s[i] == 'E') {
        i++;
        if i < s.len && (s[i] == '+' || s[i] == '-') { i++; }
        let d = i;
        while i < s.len && s[i] >= '0' && s[i] <= '9' { i++; }
        if i == d { return 0.0, false; }
    }
    let v, ok = parse_flt(s[start..i]);
    if !ok { return 0.0, false; }
    p = i;
    return v, true;
}

// Skips one value at s[p] (after whitespace), validating it; nested containers are walked with
// an explicit stack, so no input can recurse.
fn skip_value(s: const u8[:], p: i64&) -> bool {
    var stack: bool[512] = [false; 512]; // true: an object
    var depth = 0;
    loop {
        // A value is expected here.
        ws(s, p);
        if p >= s.len { return false; }
        let c = s[p];
        var opened = false;
        if c == '{' || c == '[' {
            p++;
            ws(s, p);
            if p >= s.len { return false; }
            let close: u8 = if c == '{' { '}' } else { ']' };
            if s[p] == close {
                p++;
            } else {
                if depth >= 512 { return false; }
                stack[depth] = c == '{';
                depth++;
                opened = true;
                if c == '{' {
                    if s[p] != '"' { return false; }
                    let a, b, e, ok = read_string(s, p);
                    if !ok { return false; }
                    ws(s, p);
                    if p >= s.len || s[p] != ':' { return false; }
                    p++;
                }
            }
        } else if c == '"' {
            let a, b, e, ok = read_string(s, p);
            if !ok { return false; }
        } else if c == 't' {
            if !lit(s, p, "true") { return false; }
        } else if c == 'f' {
            if !lit(s, p, "false") { return false; }
        } else if c == 'n' {
            if !lit(s, p, "null") { return false; }
        } else {
            let v, ok = read_number(s, p);
            if !ok { return false; }
        }
        if opened { continue; }
        // After a value: close containers, or move to the next element.
        var more = false;
        while depth > 0 && !more {
            ws(s, p);
            if p >= s.len { return false; }
            let d = s[p];
            let obj = stack[depth - 1];
            if d == ',' {
                p++;
                if obj {
                    ws(s, p);
                    if p >= s.len || s[p] != '"' { return false; }
                    let a, b, e, ok = read_string(s, p);
                    if !ok { return false; }
                    ws(s, p);
                    if p >= s.len || s[p] != ':' { return false; }
                    p++;
                }
                more = true;
            } else if (obj && d == '}') || (!obj && d == ']') {
                p++;
                depth--;
            } else {
                return false;
            }
        }
        if !more { return true; }
    }
}

// Walks a whole JSON document: for each member of a top-level object F(key) is called with p at
// the member's value, which F must consume. 1 an object, 0 valid JSON that is not an object, -1
// invalid. Escaped member names are decoded (into kbuf) before F sees them.
fn object_members<F>(s: const u8[:], p: i64&, kbuf: u8[>..]&) -> i32 {
    p = 0;
    ws(s, p);
    if p >= s.len { return -1; }
    if s[p] != '{' {
        if !skip_value(s, p) { return -1; }
        ws(s, p);
        return if p == s.len { 0 } else { -1 };
    }
    p++;
    ws(s, p);
    if p < s.len && s[p] == '}' {
        p++;
    } else {
        loop {
            ws(s, p);
            if p >= s.len || s[p] != '"' { return -1; }
            let ks, ke, kesc, ok = read_string(s, p);
            if !ok { return -1; }
            ws(s, p);
            if p >= s.len || s[p] != ':' { return -1; }
            p++;
            ws(s, p);
            if kesc {
                kbuf.clear();
                unescape(s[ks..ke], kbuf);
                F(kbuf[..]);
            } else {
                F(s[ks..ke]);
            }
            ws(s, p);
            if p >= s.len { return -1; }
            if s[p] == ',' { p++; continue; }
            if s[p] == '}' { p++; break; }
            return -1;
        }
    }
    ws(s, p);
    return if p == s.len { 1 } else { -1 };
}
