package main

import (
	"bytes"
	"net/url"
	"strconv"
	"strings"
	"time"
)

const maxHeader = 16384
const maxBody = 16384
const maxInput = 65536 // also caps chunk framing and trailers

type request struct {
	job                    job
	close, proto10, expect bool
}
type parseError struct {
	status int
	detail string
}

func token(s string) bool {
	if s == "" {
		return false
	}
	for _, c := range s {
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || strings.ContainsRune("!#$%&'*+-.^_`|~", c)) {
			return false
		}
	}
	return true
}
func hasToken(s, want string) bool {
	for _, p := range strings.Split(s, ",") {
		if strings.EqualFold(strings.TrimSpace(p), want) {
			return true
		}
	}
	return false
}
func parseRequest(b []byte) (r request, consumed int, e *parseError) {
	bad := func(status int) (request, int, *parseError) { return r, 0, &parseError{status: status} }
	end := bytes.Index(b, []byte("\r\n\r\n"))
	if end < 0 {
		if len(b) > maxHeader {
			return bad(431)
		}
		return
	}
	if end+4 > maxHeader {
		return bad(431)
	}
	lines := bytes.Split(b[:end], []byte("\r\n"))
	parts := strings.Split(string(lines[0]), " ")
	if len(parts) != 3 || !token(parts[0]) || (parts[2] != "HTTP/1.1" && parts[2] != "HTTP/1.0") {
		return bad(400)
	}
	u, err := url.ParseRequestURI(parts[1])
	if err != nil || u.Fragment != "" {
		return bad(400)
	}
	r.job.method, r.job.path = parts[0], u.Path
	r.proto10 = parts[2] == "HTTP/1.0"
	r.close = r.proto10
	length := -1
	host := false
	te := ""
	connection := ""
	for _, line := range lines[1:] {
		i := bytes.IndexByte(line, ':')
		if i < 1 || !token(string(line[:i])) {
			return bad(400)
		}
		value := strings.Trim(string(line[i+1:]), " \t")
		for _, c := range value {
			if c < 32 && c != '\t' || c == 127 {
				return bad(400)
			}
		}
		switch strings.ToLower(string(line[:i])) {
		case "host":
			if host || value == "" || strings.ContainsAny(value, " /\t") {
				return bad(400)
			}
			host = true
		case "authorization":
			if r.job.auth == "" {
				r.job.auth = value
			}
		case "content-length":
			if value == "" {
				return bad(400)
			}
			for _, c := range value {
				if c < '0' || c > '9' {
					return bad(400)
				}
			}
			n, err := strconv.ParseUint(value, 10, 63)
			if err != nil {
				return bad(400)
			}
			if length >= 0 && uint64(length) != n {
				return bad(400)
			}
			length = int(n)
		case "transfer-encoding":
			if te != "" {
				return bad(400)
			}
			te = value
		case "connection":
			connection += "," + value
		case "expect":
			if !strings.EqualFold(value, "100-continue") {
				return bad(417)
			}
			r.expect = true
		}
	}
	if !host && !r.proto10 && u.Host == "" {
		return r, 0, &parseError{status: 400, detail: "missing required Host header"}
	}
	if hasToken(connection, "keep-alive") && r.proto10 {
		r.close = false
	}
	if hasToken(connection, "close") {
		r.close = true
	}
	start := end + 4
	if te != "" {
		if !strings.EqualFold(te, "chunked") || r.proto10 {
			return bad(501)
		}
		body, n, status := chunkBody(b[start:])
		if status != 0 {
			return bad(status)
		}
		if n == 0 {
			return
		}
		r.job.body = body
		return r, start + n, nil
	}
	if length < 0 {
		length = 0
	}
	r.expect = r.expect && length > 0
	if length > maxBody {
		return bad(413)
	}
	if len(b)-start < length {
		return
	}
	r.job.body = b[start : start+length]
	return r, start + length, nil
}

func chunkBody(b []byte) ([]byte, int, int) {
	pos, total := 0, 0
	type span struct{ start, end int }
	var spans []span
	for {
		if len(b) > maxInput {
			return nil, 0, 413
		}
		end := bytes.Index(b[pos:], []byte("\r\n"))
		if end < 0 {
			return nil, 0, 0
		}
		end += pos
		size := string(b[pos:end])
		if i := strings.IndexByte(size, ';'); i >= 0 {
			size = size[:i]
		}
		if size == "" {
			return nil, 0, 400
		}
		for _, c := range size {
			if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f' || c >= 'A' && c <= 'F') {
				return nil, 0, 400
			}
		}
		n, err := strconv.ParseUint(size, 16, 63)
		if err != nil {
			return nil, 0, 400
		}
		pos = end + 2
		if n == 0 {
			for {
				end = bytes.Index(b[pos:], []byte("\r\n"))
				if end < 0 {
					return nil, 0, 0
				}
				end += pos
				if end == pos {
					pos += 2
					break
				}
				line := b[pos:end]
				i := bytes.IndexByte(line, ':')
				if i < 1 || !token(string(line[:i])) {
					return nil, 0, 400
				}
				pos = end + 2
			}
			body := make([]byte, 0, total)
			for _, s := range spans {
				body = append(body, b[s.start:s.end]...)
			}
			return body, pos, 0
		}
		if n > uint64(maxBody-total) {
			return nil, 0, 413
		}
		if uint64(len(b)-pos) < n+2 {
			return nil, 0, 0
		}
		next := pos + int(n)
		if b[next] != '\r' || b[next+1] != '\n' {
			return nil, 0, 400
		}
		spans = append(spans, span{pos, next})
		total += int(n)
		pos = next + 2
	}
}

var statusText = map[int]string{200: "OK", 201: "Created", 400: "Bad Request", 401: "Unauthorized", 404: "Not Found", 413: "Request Entity Too Large", 417: "Expectation Failed", 431: "Request Header Fields Too Large", 500: "Internal Server Error", 501: "Not Implemented", 503: "Service Unavailable"}

func appendResponse(dst []byte, r reply, req request, now time.Time, protocolError bool) []byte {
	proto := "HTTP/1.1 "
	if req.proto10 {
		proto = "HTTP/1.0 "
	}
	dst = append(dst, proto...)
	dst = strconv.AppendInt(dst, int64(r.status), 10)
	dst = append(dst, ' ')
	dst = append(dst, statusText[r.status]...)
	if protocolError {
		text := string(r.data)
		if i := strings.Index(text, ": "); i >= 0 {
			dst = append(dst, text[i:]...)
		}
	}
	dst = append(dst, "\r\n"...)
	if protocolError && r.status == 417 {
		dst = append(dst, "Connection: close\r\nDate: "...)
		dst = now.UTC().AppendFormat(dst, "Mon, 02 Jan 2006 15:04:05 GMT")
		dst = append(dst, "\r\nContent-Length: 0\r\n\r\n"...)
		return dst
	}
	if protocolError {
		// net/http's transport-level error response has no Date and closes.
		dst = append(dst, "Content-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n"...)
	} else {
		if r.status == 413 {
			dst = append(dst, "Connection: close\r\n"...)
		}
		dst = append(dst, "Content-Length: "...)
		dst = strconv.AppendInt(dst, int64(len(r.data)), 10)
		dst = append(dst, "\r\nContent-Type: application/json\r\nDate: "...)
		dst = now.UTC().AppendFormat(dst, "Mon, 02 Jan 2006 15:04:05 GMT")
		dst = append(dst, "\r\n"...)
		if req.close && !req.proto10 && r.status != 413 {
			dst = append(dst, "Connection: close\r\n"...)
		} else if req.proto10 && !req.close {
			dst = append(dst, "Connection: keep-alive\r\n"...)
		}
		dst = append(dst, "\r\n"...)
	}
	if req.job.method != "HEAD" {
		dst = append(dst, r.data...)
	}
	return dst
}

func protocolErrorText(e *parseError) string {
	if e.status == 501 {
		return "Unsupported transfer encoding"
	}
	s := strconv.Itoa(e.status) + " " + statusText[e.status]
	if e.detail != "" {
		s += ": " + e.detail
	}
	return s
}
