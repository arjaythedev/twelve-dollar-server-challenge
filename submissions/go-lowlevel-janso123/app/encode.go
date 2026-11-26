package main

import (
	"strconv"
	"unicode/utf8"
)

// appendJSONQuote matches encoding/json's default HTML-safe string spelling.
func appendJSONQuote(dst []byte, s string) []byte {
	const hex = "0123456789abcdef"
	dst = append(dst, '"')
	for i := 0; i < len(s); {
		c := s[i]
		if c < utf8.RuneSelf {
			switch c {
			case '"', '\\':
				dst = append(dst, '\\', c)
			case '\b':
				dst = append(dst, '\\', 'b')
			case '\f':
				dst = append(dst, '\\', 'f')
			case '\n':
				dst = append(dst, '\\', 'n')
			case '\r':
				dst = append(dst, '\\', 'r')
			case '\t':
				dst = append(dst, '\\', 't')
			case '<':
				dst = append(dst, `\u003c`...)
			case '>':
				dst = append(dst, `\u003e`...)
			case '&':
				dst = append(dst, `\u0026`...)
			default:
				if c < 0x20 {
					dst = append(dst, '\\', 'u', '0', '0', hex[c>>4], hex[c&15])
				} else {
					dst = append(dst, c)
				}
			}
			i++
			continue
		}
		r, n := utf8.DecodeRuneInString(s[i:])
		if n == 1 {
			dst = append(dst, '\xef', '\xbf', '\xbd')
		} else if r == '\u2028' {
			dst = append(dst, `\u2028`...)
		} else if r == '\u2029' {
			dst = append(dst, `\u2029`...)
		} else {
			dst = append(dst, s[i:i+n]...)
		}
		i += n
	}
	return append(dst, '"')
}

func appendPostJSON(dst []byte, p Post) []byte {
	dst = append(dst, `{"id":`...)
	dst = strconv.AppendInt(dst, p.ID, 10)
	dst = append(dst, `,"body":`...)
	dst = appendJSONQuote(dst, p.Body)
	dst = append(dst, `,"created_at":`...)
	dst = appendJSONQuote(dst, p.Created)
	dst = append(dst, `,"author":`...)
	dst = appendJSONQuote(dst, p.Author)
	dst = append(dst, `,"like_count":`...)
	dst = strconv.AppendInt(dst, p.Likes, 10)
	return append(dst, '}')
}

func createdPost(p Post) reply {
	dst := appendPostJSON([]byte(`{"post":`), p)
	return reply{201, append(dst, '}')}
}

func likeResult(status int, already bool, id int64) reply {
	dst := []byte(`{"liked":true,"already_liked":`)
	if already {
		dst = append(dst, "true"...)
	} else {
		dst = append(dst, "false"...)
	}
	dst = append(dst, `,"post_id":`...)
	dst = strconv.AppendInt(dst, id, 10)
	return reply{status, append(dst, '}')}
}

func failure(status int, message string) reply {
	dst := appendJSONQuote([]byte(`{"error":`), message)
	return reply{status, append(dst, '}')}
}

func healthOK(uptime int64) reply {
	dst := strconv.AppendInt([]byte(`{"status":"ok","db":"ok","uptime_s":`), uptime, 10)
	return reply{200, append(dst, '}')}
}

func healthError(message string) reply {
	dst := appendJSONQuote([]byte(`{"status":"degraded","db":"unreachable","error":`), message)
	return reply{503, append(dst, '}')}
}
