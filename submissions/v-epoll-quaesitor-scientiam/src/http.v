module main

import fasthttp

// Req is a parsed request: byte ranges into the connection's read buffer, no copies.
pub type Req = fasthttp.HttpRequest

@[inline]
fn lower(c u8) u8 {
	return if c >= `A` && c <= `Z` { c + 32 } else { c }
}

// bytes_eq compares a request slice with a literal.
fn bytes_eq(buf []u8, s fasthttp.Slice, lit string) bool {
	if s.len != lit.len {
		return false
	}
	for k in 0 .. lit.len {
		if buf[s.start + k] != lit[k] {
			return false
		}
	}
	return true
}

// header_value finds `name: value` in the header block (name compared case-insensitively,
// value trimmed of surrounding spaces/tabs) and returns the value as a slice, or len -1.
fn header_value(buf []u8, hf fasthttp.Slice, name string) fasthttp.Slice {
	mut i := hf.start
	end := hf.start + hf.len
	for i < end {
		// name
		mut j := i
		mut matched := true
		for k in 0 .. name.len {
			if j >= end || lower(buf[j]) != name[k] {
				matched = false
				break
			}
			j++
		}
		if matched && j < end && buf[j] == `:` {
			j++
			for j < end && (buf[j] == ` ` || buf[j] == `\t`) {
				j++
			}
			mut e := j
			for e < end && buf[e] != `\r` && buf[e] != `\n` {
				e++
			}
			mut t := e
			for t > j && (buf[t - 1] == ` ` || buf[t - 1] == `\t`) {
				t--
			}
			return fasthttp.Slice{
				start: j
				len:   t - j
			}
		}
		// skip to the next line
		for i < end && buf[i] != `\n` {
			i++
		}
		i++
	}
	return fasthttp.Slice{
		start: 0
		len:   -1
	}
}

// bearer_token returns the token after `Bearer ` in the Authorization header, or len -1
// when the header is absent or has another scheme.
fn bearer_token(buf []u8, hf fasthttp.Slice) fasthttp.Slice {
	v := header_value(buf, hf, 'authorization')
	if v.len < 7 {
		return fasthttp.Slice{0, -1}
	}
	b := buf[v.start..v.start + 7]
	if lower(b[0]) != `b` || lower(b[1]) != `e` || lower(b[2]) != `a` || lower(b[3]) != `r`
		|| lower(b[4]) != `e` || lower(b[5]) != `r` || b[6] != ` ` {
		return fasthttp.Slice{0, -1}
	}
	mut s := v.start + 7
	e := v.start + v.len
	for s < e && buf[s] == ` ` {
		s++
	}
	return fasthttp.Slice{
		start: s
		len:   e - s
	}
}

// parse_post_id parses the `:id` path segment: digits only, positive, no leading junk.
// Anything else (0, -1, abc, 1.5) is -1.
fn parse_post_id(buf []u8, start int, end int) i64 {
	if end <= start || end - start > 18 {
		return -1
	}
	mut v := i64(0)
	for k in start .. end {
		c := buf[k]
		if c < `0` || c > `9` {
			return -1
		}
		v = v * 10 + i64(c - `0`)
	}
	return if v > 0 { v } else { -1 }
}
