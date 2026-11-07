package main

import "strconv"

// ---- output -----------------------------------------------------------------------------------

// needsEsc marks the bytes JSON strings must escape.
var needsEsc = func() (t [256]bool) {
	for i := 0; i < 0x20; i++ {
		t[i] = true
	}
	t['"'] = true
	t['\\'] = true
	return
}()

const hexDigits = "0123456789abcdef"

const (
	lo7 = 0x0101010101010101
	hi7 = 0x8080808080808080
)

// plain8 reports whether none of the 8 bytes in w needs escaping (no '"', '\\' or control
// character). Bytes >= 0x80 never need it.
func plain8(w uint64) bool {
	ctl := (w - lo7*0x20) &^ w // high bit set where a byte < 0x20 (or a borrow from one)
	q := w ^ (lo7 * '"')
	quo := (q - lo7) &^ q // high bit where a byte == '"'
	bs := w ^ (lo7 * '\\')
	bsl := (bs - lo7) &^ bs // high bit where a byte == '\\'
	return (ctl|quo|bsl)&hi7 == 0
}

// appendEsc appends s as the inside of a JSON string. Text without anything to escape (the
// usual case) is checked 8 bytes at a time and copied in one piece.
func appendEsc(b, s []byte) []byte {
	i := 0
	for ; i+8 <= len(s); i += 8 {
		if !plain8(le64(s[i:])) {
			break
		}
	}
	for ; i < len(s) && !needsEsc[s[i]]; i++ {
	}
	if i == len(s) {
		return append(b, s...)
	}
	return appendEscSlow(b, s)
}

func le64(b []byte) uint64 {
	_ = b[7]
	return uint64(b[0]) | uint64(b[1])<<8 | uint64(b[2])<<16 | uint64(b[3])<<24 |
		uint64(b[4])<<32 | uint64(b[5])<<40 | uint64(b[6])<<48 | uint64(b[7])<<56
}

func appendEscSlow(b, s []byte) []byte {
	start := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		if !needsEsc[c] {
			continue
		}
		b = append(b, s[start:i]...)
		switch c {
		case '"':
			b = append(b, '\\', '"')
		case '\\':
			b = append(b, '\\', '\\')
		case '\n':
			b = append(b, '\\', 'n')
		case '\r':
			b = append(b, '\\', 'r')
		case '\t':
			b = append(b, '\\', 't')
		case '\b':
			b = append(b, '\\', 'b')
		case '\f':
			b = append(b, '\\', 'f')
		default:
			b = append(b, '\\', 'u', '0', '0', hexDigits[c>>4], hexDigits[c&15])
		}
		start = i + 1
	}
	return append(b, s[start:]...)
}

func appendInt(b []byte, v int64) []byte {
	if uint64(v) < 10 {
		return append(b, byte('0'+v))
	}
	return strconv.AppendInt(b, v, 10)
}
