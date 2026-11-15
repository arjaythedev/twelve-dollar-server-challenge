module main

// A small validating JSON reader over a byte buffer. It walks a document once, decodes the
// string values the handlers need into caller-owned scratch buffers and skips everything
// else. It never allocates.

const json_max_depth = 32

struct JsonReader {
	b []u8
mut:
	i int
}

@[inline]
fn is_json_ws(c u8) bool {
	return c == ` ` || c == `\n` || c == `\r` || c == `\t`
}

fn (mut r JsonReader) skip_ws() {
	for r.i < r.b.len && is_json_ws(r.b[r.i]) {
		r.i++
	}
}

@[inline]
fn (r &JsonReader) peek() u8 {
	return if r.i < r.b.len { r.b[r.i] } else { 0 }
}

fn hex_val(c u8) int {
	if c >= `0` && c <= `9` {
		return c - `0`
	}
	if c >= `a` && c <= `f` {
		return c - `a` + 10
	}
	if c >= `A` && c <= `F` {
		return c - `A` + 10
	}
	return -1
}

fn (mut r JsonReader) read_hex4() !int {
	if r.i + 4 > r.b.len {
		return error('eof')
	}
	mut v := u32(0)
	for k in 0 .. 4 {
		h := hex_val(r.b[r.i + k])
		if h < 0 {
			return error('hex')
		}
		v = v << 4 | u32(h)
	}
	r.i += 4
	return int(v)
}

fn put_utf8(mut dst []u8, cp int) {
	if cp < 0x80 {
		dst << u8(cp)
	} else if cp < 0x800 {
		dst << u8(0xC0 | cp >> 6)
		dst << u8(0x80 | cp & 0x3F)
	} else if cp < 0x10000 {
		dst << u8(0xE0 | cp >> 12)
		dst << u8(0x80 | cp >> 6 & 0x3F)
		dst << u8(0x80 | cp & 0x3F)
	} else {
		dst << u8(0xF0 | cp >> 18)
		dst << u8(0x80 | cp >> 12 & 0x3F)
		dst << u8(0x80 | cp >> 6 & 0x3F)
		dst << u8(0x80 | cp & 0x3F)
	}
}

// utf8_len returns the length of the well-formed UTF-8 sequence starting at b[i] (whose
// first byte is >= 0x80), or 0 when it is malformed: a stray continuation byte, an overlong
// form, a surrogate, a code point above U+10FFFF or a truncated sequence.
fn utf8_len(b []u8, i int) int {
	c := b[i]
	n := if c < 0xC2 {
		0
	} else if c < 0xE0 {
		2
	} else if c < 0xF0 {
		3
	} else if c < 0xF5 {
		4
	} else {
		0
	}
	if n == 0 || i + n > b.len {
		return 0
	}
	for k in 1 .. n {
		if b[i + k] & 0xC0 != 0x80 {
			return 0
		}
	}
	c1 := b[i + 1]
	if (c == 0xE0 && c1 < 0xA0) || (c == 0xF0 && c1 < 0x90) {
		return 0 // overlong
	}
	if (c == 0xED && c1 >= 0xA0) || (c == 0xF4 && c1 >= 0x90) {
		return 0 // surrogate / above U+10FFFF
	}
	return n
}

// string_into reads a JSON string (r.i at the opening quote) and appends its decoded
// bytes to dst. With dst unset (nil data) the string is only validated. Raw bytes must be
// well-formed UTF-8 (RFC 8259 §8.1); anything else is not JSON.
fn (mut r JsonReader) string_into(mut dst []u8, decode bool) ! {
	if r.peek() != `"` {
		return error('string')
	}
	r.i++
	mut run := r.i
	for {
		if r.i >= r.b.len {
			return error('eof')
		}
		c := r.b[r.i]
		if c == `"` {
			if decode {
				put_bytes(mut dst, unsafe { &u8(r.b.data) + run }, r.i - run)
			}
			r.i++
			return
		}
		if c < 0x20 {
			return error('control')
		}
		if c >= 0x80 {
			n := utf8_len(r.b, r.i)
			if n == 0 {
				return error('utf8')
			}
			r.i += n
			continue
		}
		if c != `\\` {
			r.i++
			continue
		}
		if decode {
			put_bytes(mut dst, unsafe { &u8(r.b.data) + run }, r.i - run)
		}
		r.i++
		if r.i >= r.b.len {
			return error('eof')
		}
		e := r.b[r.i]
		r.i++
		mut cp := -1
		match e {
			`"`, `\\`, `/` { cp = int(e) }
			`n` { cp = int(`\n`) }
			`r` { cp = int(`\r`) }
			`t` { cp = int(`\t`) }
			`b` { cp = int(`\b`) }
			`f` { cp = int(`\f`) }
			`u` {
				cp = r.read_hex4()!
				if cp >= 0xD800 && cp <= 0xDBFF {
					// high surrogate: a low surrogate must follow
					if r.i + 6 > r.b.len || r.b[r.i] != `\\` || r.b[r.i + 1] != `u` {
						return error('surrogate')
					}
					r.i += 2
					lo := r.read_hex4()!
					if lo < 0xDC00 || lo > 0xDFFF {
						return error('surrogate')
					}
					cp = 0x10000 + (cp - 0xD800) * 0x400 + (lo - 0xDC00)
				} else if cp >= 0xDC00 && cp <= 0xDFFF {
					return error('surrogate')
				}
			}
			else { return error('escape') }
		}
		if decode {
			put_utf8(mut dst, cp)
		}
		run = r.i
	}
}

fn (mut r JsonReader) literal(lit string) ! {
	if r.i + lit.len > r.b.len {
		return error('eof')
	}
	for k in 0 .. lit.len {
		if r.b[r.i + k] != lit[k] {
			return error('literal')
		}
	}
	r.i += lit.len
}

fn (mut r JsonReader) number() ! {
	start := r.i
	if r.peek() == `-` {
		r.i++
	}
	mut digits := 0
	for r.i < r.b.len && r.b[r.i] >= `0` && r.b[r.i] <= `9` {
		r.i++
		digits++
	}
	if digits == 0 {
		return error('number')
	}
	if r.peek() == `.` {
		r.i++
		mut frac := 0
		for r.i < r.b.len && r.b[r.i] >= `0` && r.b[r.i] <= `9` {
			r.i++
			frac++
		}
		if frac == 0 {
			return error('number')
		}
	}
	if r.peek() == `e` || r.peek() == `E` {
		r.i++
		if r.peek() == `+` || r.peek() == `-` {
			r.i++
		}
		mut exp := 0
		for r.i < r.b.len && r.b[r.i] >= `0` && r.b[r.i] <= `9` {
			r.i++
			exp++
		}
		if exp == 0 {
			return error('number')
		}
	}
	_ = start
}

// skip_value validates and skips any JSON value.
fn (mut r JsonReader) skip_value(depth int) ! {
	if depth > json_max_depth {
		return error('depth')
	}
	r.skip_ws()
	mut none_ := []u8{}
	match r.peek() {
		`"` {
			r.string_into(mut none_, false)!
		}
		`{` {
			r.i++
			r.skip_ws()
			if r.peek() == `}` {
				r.i++
				return
			}
			for {
				r.skip_ws()
				r.string_into(mut none_, false)!
				r.skip_ws()
				if r.peek() != `:` {
					return error('colon')
				}
				r.i++
				r.skip_value(depth + 1)!
				r.skip_ws()
				c := r.peek()
				r.i++
				if c == `}` {
					return
				}
				if c != `,` {
					return error('comma')
				}
			}
		}
		`[` {
			r.i++
			r.skip_ws()
			if r.peek() == `]` {
				r.i++
				return
			}
			for {
				r.skip_value(depth + 1)!
				r.skip_ws()
				c := r.peek()
				r.i++
				if c == `]` {
					return
				}
				if c != `,` {
					return error('comma')
				}
			}
		}
		`t` {
			r.literal('true')!
		}
		`f` {
			r.literal('false')!
		}
		`n` {
			r.literal('null')!
		}
		else {
			r.number()!
		}
	}
}

// key_is reports whether the raw key bytes [start,end) equal name (no escapes considered).
fn key_is(b []u8, start int, end int, name string) bool {
	if end - start != name.len {
		return false
	}
	for k in 0 .. name.len {
		if b[start + k] != name[k] {
			return false
		}
	}
	return true
}

// ObjectWalker iterates the members of a top-level JSON object. After each next() the
// caller inspects key bytes [key_start,key_end) and reads or skips the value.
struct ObjectWalker {
mut:
	r         JsonReader
	key_start int
	key_end   int
	first     bool = true
	done      bool
}

fn new_object_walker(b []u8) !ObjectWalker {
	mut w := ObjectWalker{
		r: JsonReader{
			b: b
		}
	}
	w.r.skip_ws()
	if w.r.peek() != `{` {
		return error('object')
	}
	w.r.i++
	return w
}

// next positions the walker at the next member's value. It returns false at the end of
// the object (after checking that nothing but whitespace follows it).
fn (mut w ObjectWalker) next() !bool {
	if w.done {
		return false
	}
	w.r.skip_ws()
	if w.first {
		w.first = false
		if w.r.peek() == `}` {
			w.r.i++
			return w.finish()
		}
	} else {
		c := w.r.peek()
		w.r.i++
		if c == `}` {
			return w.finish()
		}
		if c != `,` {
			return error('comma')
		}
		w.r.skip_ws()
	}
	if w.r.peek() != `"` {
		return error('key')
	}
	w.key_start = w.r.i + 1
	mut none_ := []u8{}
	w.r.string_into(mut none_, false)!
	w.key_end = w.r.i - 1
	w.r.skip_ws()
	if w.r.peek() != `:` {
		return error('colon')
	}
	w.r.i++
	w.r.skip_ws()
	return true
}

fn (mut w ObjectWalker) finish() !bool {
	w.done = true
	w.r.skip_ws()
	if w.r.i != w.r.b.len {
		return error('trailing')
	}
	return false
}

fn (w &ObjectWalker) key(name string) bool {
	return key_is(w.r.b, w.key_start, w.key_end, name)
}

// value_is_string reports whether the current value is a JSON string.
fn (w &ObjectWalker) value_is_string() bool {
	return w.r.peek() == `"`
}

// read_string decodes the current string value into dst (which is cleared first).
fn (mut w ObjectWalker) read_string(mut dst []u8) ! {
	dst.clear()
	w.r.string_into(mut dst, true)!
}

// read_int reads the current value as an integer. A JSON number with a fraction or
// exponent is accepted and truncated; anything else is an error.
fn (mut w ObjectWalker) read_int() !i64 {
	start := w.r.i
	w.r.number()!
	mut v := i64(0)
	mut k := start
	neg := w.r.b[k] == `-`
	if neg {
		k++
	}
	for k < w.r.i && w.r.b[k] >= `0` && w.r.b[k] <= `9` {
		d := i64(w.r.b[k] - `0`)
		if v > (max_i64 - d) / 10 {
			return error('overflow')
		}
		v = v * 10 + d
		k++
	}
	return if neg { -v } else { v }
}

// skip skips the current value.
fn (mut w ObjectWalker) skip() ! {
	w.r.skip_value(1)!
}
