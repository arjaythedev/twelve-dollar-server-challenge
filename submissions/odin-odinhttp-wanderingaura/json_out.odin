package main

import "base:intrinsics"
import "core:simd"

// Responses are written into one preallocated buffer (OUT_BUF_SIZE) and copied into the
// response once; it is large enough for the worst case feed (20 bodies of 500 control
// characters escaped to 6 bytes each).

Out :: struct {
	buf: [^]byte,
	len: int,
}

out: Out

out_reset :: #force_inline proc() {
	out.len = 0
}

out_bytes :: #force_inline proc() -> []byte {
	return out.buf[:out.len]
}

write_str :: #force_inline proc(s: string) {
	intrinsics.mem_copy_non_overlapping(&out.buf[out.len], raw_data(s), len(s))
	out.len += len(s)
}

write_raw :: #force_inline proc(s: []byte) {
	intrinsics.mem_copy_non_overlapping(&out.buf[out.len], raw_data(s), len(s))
	out.len += len(s)
}

@(private = "file")
DIGIT_PAIRS := "00010203040506070809101112131415161718192021222324252627282930313233343536373839" +
	"40414243444546474849505152535455565758596061626364656667686970717273747576777879" +
	"8081828384858687888990919293949596979899"

write_int :: proc(v: i64) {
	if v < 0 {
		out.buf[out.len] = '-'
		out.len += 1
		write_uint(u64(-v))
	} else {
		write_uint(u64(v))
	}
}

write_uint :: proc(v: u64) {
	tmp: [20]byte
	i := len(tmp)
	v := v
	for v >= 100 {
		d := (v % 100) * 2
		v /= 100
		i -= 2
		tmp[i] = DIGIT_PAIRS[d]
		tmp[i + 1] = DIGIT_PAIRS[d + 1]
	}
	if v >= 10 {
		d := v * 2
		i -= 2
		tmp[i] = DIGIT_PAIRS[d]
		tmp[i + 1] = DIGIT_PAIRS[d + 1]
	} else {
		i -= 1
		tmp[i] = '0' + u8(v)
	}
	write_raw(tmp[i:])
}

@(private = "file")
HEX := "0123456789abcdef"

@(private = "file")
write_escaped_byte :: #force_inline proc(b: u8) {
	p := out.buf[out.len:]
	switch b {
	case '"':  p[0] = '\\'; p[1] = '"';  out.len += 2
	case '\\': p[0] = '\\'; p[1] = '\\'; out.len += 2
	case '\n': p[0] = '\\'; p[1] = 'n';  out.len += 2
	case '\r': p[0] = '\\'; p[1] = 'r';  out.len += 2
	case '\t': p[0] = '\\'; p[1] = 't';  out.len += 2
	case '\b': p[0] = '\\'; p[1] = 'b';  out.len += 2
	case '\f': p[0] = '\\'; p[1] = 'f';  out.len += 2
	case 0..<0x20:
		p[0] = '\\'; p[1] = 'u'; p[2] = '0'; p[3] = '0'
		p[4] = HEX[b >> 4]; p[5] = HEX[b & 0xf]
		out.len += 6
	case:
		p[0] = b
		out.len += 1
	}
}

V16 :: #simd[16]u8

// Writes s as a JSON string literal. 16 bytes at a time: a chunk with no quote, backslash or
// control character is copied as is, otherwise that chunk is escaped byte by byte.
write_json_string :: proc(s: []byte) {
	out.buf[out.len] = '"'
	out.len += 1

	i := 0
	n := len(s)
	for ; i + 16 <= n; i += 16 {
		v := intrinsics.unaligned_load((^V16)(&s[i]))
		special := simd.lanes_lt(v, V16(0x20)) | simd.lanes_eq(v, V16('"')) | simd.lanes_eq(v, V16('\\'))
		if simd.reduce_or(special) == 0 {
			intrinsics.unaligned_store((^V16)(&out.buf[out.len]), v)
			out.len += 16
		} else {
			for b in s[i:i + 16] { write_escaped_byte(b) }
		}
	}
	for b in s[i:] { write_escaped_byte(b) }

	out.buf[out.len] = '"'
	out.len += 1
}

// {"id":..,"body":..,"created_at":..,"author":..,"like_count":..}
write_post :: proc(id: i64, body, created_at, author: []byte, like_count: i64) {
	write_str(`{"id":`)
	write_int(id)
	write_str(`,"body":`)
	write_json_string(body)
	write_str(`,"created_at":"`)
	write_raw(created_at) // stored in the wire format, digits and punctuation only
	write_str(`","author":`)
	write_json_string(author)
	write_str(`,"like_count":`)
	write_int(like_count)
	write_str(`}`)
}
