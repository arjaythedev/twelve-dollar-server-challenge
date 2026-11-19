package main

import "base:intrinsics"
import "core:simd"
import "core:unicode/utf8"

// The whitespace JavaScript's String.prototype.trim removes.
is_js_space :: proc(r: rune) -> bool {
	switch r {
	case '\t', '\n', '\v', '\f', '\r', ' ', 0xA0, 0x1680, 0x2000 ..= 0x200A, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000, 0xFEFF:
		return true
	}
	return false
}

js_trim :: proc(s: string) -> string {
	s := s
	for len(s) > 0 {
		r, n := rune(s[0]), 1
		if r >= utf8.RUNE_SELF { r, n = utf8.decode_rune_in_string(s) }
		if !is_js_space(r) { break }
		s = s[n:]
	}
	for len(s) > 0 {
		r, n := rune(s[len(s) - 1]), 1
		if r >= utf8.RUNE_SELF { r, n = utf8.decode_last_rune_in_string(s) }
		if !is_js_space(r) { break }
		s = s[:len(s) - n]
	}
	return s
}

// Code points in valid UTF-8 (what SQLite's length() counts): every byte that is not a
// continuation byte (10xxxxxx) starts one. Counted 16 bytes at a time.
count_code_points :: proc(s: string) -> int {
	count := 0
	i := 0
	for ; i + 16 <= len(s); i += 16 {
		v := intrinsics.unaligned_load((^V16)(raw_data(s[i:])))
		continuation := simd.lanes_eq(v & V16(0xC0), V16(0x80))
		starts := simd.select(continuation, V16(0), V16(1))
		count += int(simd.reduce_add_pairs(starts))
	}
	for ; i < len(s); i += 1 {
		if s[i] & 0xC0 != 0x80 { count += 1 }
	}
	return count
}
