package main

import (
	"bytes"
	"strconv"

	"github.com/panjf2000/gnet/v2"
)

// One event loop on the one vCPU: no goroutine per connection. Handlers (and SQLite) run inline in the loop.
// ponytail: a WAL checkpoint fsync stalls the loop for a few ms; move writes off-loop if p99 shows it.
type httpServer struct {
	gnet.BuiltinEventEngine
	body, out []byte // reused: the loop is single-threaded
}

func listen(addr string) error {
	return gnet.Run(&httpServer{}, "tcp://"+addr,
		gnet.WithMulticore(false), gnet.WithTCPNoDelay(gnet.TCPNoDelay), gnet.WithReusePort(false), gnet.WithReuseAddr(true))
}

const maxHeader = 16 << 10
const maxBody = 1 << 20

var statusText = map[int]string{200: "OK", 201: "Created", 400: "Bad Request", 401: "Unauthorized",
	404: "Not Found", 500: "Internal Server Error", 503: "Service Unavailable"}

func (s *httpServer) OnTraffic(c gnet.Conn) gnet.Action {
	in, _ := c.Peek(-1)
	s.out = s.out[:0]
	used, closeConn := 0, false
	for !closeConn {
		r, n := parseRequest(in[used:])
		if n == 0 { // incomplete: wait for more bytes
			break
		}
		if n < 0 {
			s.out = appendResponse(s.out, 400, []byte(`{"error":"bad request"}`), true)
			closeConn = true
			break
		}
		used += n
		closeConn = r.close
		var status int
		status, s.body = serve(string(r.method), string(r.path), string(r.auth), r.body, s.body[:0])
		s.out = appendResponse(s.out, status, s.body, closeConn)
	}
	c.Discard(used)
	if len(s.out) > 0 {
		c.Write(s.out)
	}
	if closeConn {
		return gnet.Close
	}
	return gnet.None
}

func appendResponse(b []byte, status int, body []byte, closeConn bool) []byte {
	b = append(b, "HTTP/1.1 "...)
	b = strconv.AppendInt(b, int64(status), 10)
	b = append(b, ' ')
	b = append(b, statusText[status]...)
	b = append(b, "\r\nContent-Type: application/json\r\nContent-Length: "...)
	b = strconv.AppendInt(b, int64(len(body)), 10)
	if closeConn {
		b = append(b, "\r\nConnection: close"...)
	}
	b = append(b, "\r\n\r\n"...)
	return append(b, body...)
}

type request struct {
	method, path, auth, body []byte
	close                    bool
}

// parseRequest parses one HTTP/1.1 request from b.
// n > 0: bytes consumed; n == 0: incomplete; n < 0: malformed.
func parseRequest(b []byte) (r request, n int) {
	end := bytes.Index(b, []byte("\r\n\r\n"))
	if end < 0 {
		if len(b) > maxHeader {
			return r, -1
		}
		return r, 0
	}
	head := b[:end]
	line, rest, _ := bytes.Cut(head, []byte("\r\n"))
	method, line, ok1 := bytes.Cut(line, []byte(" "))
	target, proto, ok2 := bytes.Cut(line, []byte(" "))
	if !ok1 || !ok2 || len(method) == 0 || len(target) == 0 || target[0] != '/' {
		return r, -1
	}
	if q := bytes.IndexByte(target, '?'); q >= 0 {
		target = target[:q]
	}
	r.method, r.path = method, target
	r.close = string(proto) == "HTTP/1.0"
	clen := 0
	for len(rest) > 0 {
		var h []byte
		h, rest, _ = bytes.Cut(rest, []byte("\r\n"))
		name, val, ok := bytes.Cut(h, []byte(":"))
		if !ok {
			return r, -1
		}
		val = bytes.TrimSpace(val)
		switch {
		case bytes.EqualFold(name, []byte("Content-Length")):
			v, err := strconv.Atoi(string(val))
			if err != nil || v < 0 || v > maxBody {
				return r, -1
			}
			clen = v
		case bytes.EqualFold(name, []byte("Authorization")):
			r.auth = val
		case bytes.EqualFold(name, []byte("Connection")):
			if bytes.EqualFold(val, []byte("close")) {
				r.close = true
			} else if bytes.EqualFold(val, []byte("keep-alive")) {
				r.close = false
			}
		case bytes.EqualFold(name, []byte("Transfer-Encoding")):
			return r, -1 // ponytail: no chunked request bodies; clients here send Content-Length
		}
	}
	total := end + 4 + clen
	if len(b) < total {
		return r, 0
	}
	r.body = b[end+4 : total]
	return r, total
}
