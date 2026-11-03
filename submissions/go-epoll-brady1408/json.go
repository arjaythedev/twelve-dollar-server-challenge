package main

import "strconv"

const hexDigits = "0123456789abcdef"

// appendJSONString appends s as a JSON string literal. Only '"', '\\' and
// control characters are escaped; '<', '>', '&', '/' and non-ASCII pass
// through, which SPEC.md allows and the golden files expect.
func appendJSONString(dst, s []byte) []byte {
	dst = append(dst, '"')
	start := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 0x20 && c != '"' && c != '\\' {
			continue
		}
		dst = append(dst, s[start:i]...)
		switch c {
		case '"', '\\':
			dst = append(dst, '\\', c)
		case '\n':
			dst = append(dst, '\\', 'n')
		case '\r':
			dst = append(dst, '\\', 'r')
		case '\t':
			dst = append(dst, '\\', 't')
		case '\b':
			dst = append(dst, '\\', 'b')
		case '\f':
			dst = append(dst, '\\', 'f')
		default:
			dst = append(dst, '\\', 'u', '0', '0', hexDigits[c>>4], hexDigits[c&0xf])
		}
		start = i + 1
	}
	dst = append(dst, s[start:]...)
	return append(dst, '"')
}

// appendPost writes one post object in SPEC.md key order.
func appendPost(dst []byte, id int64, body, createdAt, author []byte, likeCount int64) []byte {
	dst = append(dst, `{"id":`...)
	dst = strconv.AppendInt(dst, id, 10)
	dst = append(dst, `,"body":`...)
	dst = appendJSONString(dst, body)
	dst = append(dst, `,"created_at":`...)
	dst = appendJSONString(dst, createdAt)
	dst = append(dst, `,"author":`...)
	dst = appendJSONString(dst, author)
	dst = append(dst, `,"like_count":`...)
	dst = strconv.AppendInt(dst, likeCount, 10)
	return append(dst, '}')
}

func appendError(dst []byte, msg string) []byte {
	dst = append(dst, `{"error":`...)
	dst = appendJSONString(dst, []byte(msg))
	return append(dst, '}')
}

func appendLike(dst []byte, already bool, postID int64) []byte {
	dst = append(dst, `{"liked":true,"already_liked":`...)
	dst = strconv.AppendBool(dst, already)
	dst = append(dst, `,"post_id":`...)
	dst = strconv.AppendInt(dst, postID, 10)
	return append(dst, '}')
}

func appendHealthOK(dst []byte, uptime int64) []byte {
	dst = append(dst, `{"status":"ok","db":"ok","uptime_s":`...)
	dst = strconv.AppendInt(dst, uptime, 10)
	return append(dst, '}')
}
