package main

import "unicode/utf8"

type bodyError uint8

const (
	bodyOK        bodyError = iota
	bodyMalformed           // "malformed JSON body"
	bodyRequired            // "body is required"
	bodyTooLong             // "body must be at most 500 characters"
)

func (e bodyError) String() string { return [...]string{"ok", "malformed", "required", "too long"}[e] }

const maxBodyRunes = 500

// parseBody reads {"body": "..."} from raw, unescapes and trims it into *out (reset and reused)
// and returns the trimmed body. Trimming follows JavaScript's String.prototype.trim().
func parseBody(raw []byte, out *[]byte) ([]byte, bodyError) {
	var (
		val   []byte
		found bool
	)
	if !parseObject(raw, func(k, v []byte, kind byte) {
		if string(k) == "body" { // last duplicate wins, like JSON.parse
			val, found = v, kind == kindString
		}
	}) {
		return nil, bodyMalformed
	}
	if !found {
		return nil, bodyRequired
	}
	buf, ok := unescape((*out)[:0], val)
	*out = buf
	if !ok {
		return nil, bodyMalformed
	}
	buf = trimJS(buf)
	if len(buf) == 0 {
		return nil, bodyRequired
	}
	if utf8.RuneCount(buf) > maxBodyRunes {
		return nil, bodyTooLong
	}
	return buf, bodyOK
}

func isJSSpace(r rune) bool {
	switch r {
	case '\t', '\n', '\v', '\f', '\r', ' ', 0x00a0, 0x1680, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0xfeff:
		return true
	}
	return r >= 0x2000 && r <= 0x200a
}

func trimJS(b []byte) []byte {
	for len(b) > 0 {
		r, n := utf8.DecodeRune(b)
		if !isJSSpace(r) {
			break
		}
		b = b[n:]
	}
	for len(b) > 0 {
		r, n := utf8.DecodeLastRune(b)
		if !isJSSpace(r) {
			break
		}
		b = b[:len(b)-n]
	}
	return b
}
