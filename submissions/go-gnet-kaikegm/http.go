package main

import (
	"bytes"
	"strconv"
	"time"

	"github.com/panjf2000/gnet/v2"
)

// httpServer handles HTTP/1.1 connections in an epoll event loop without allocating a goroutine per connection.
type httpServer struct {
	gnet.BuiltinEventEngine
	body, out []byte // reused buffer for the single-threaded event loop
}

func listen(addr string) error {
	srv := &httpServer{
		body: make([]byte, 0, 64<<10),
		out:  make([]byte, 0, 64<<10),
	}
	return gnet.Run(srv, "tcp://"+addr,
		gnet.WithMulticore(false),
		gnet.WithTCPNoDelay(gnet.TCPNoDelay),
		gnet.WithTCPKeepAlive(65*time.Second),
		gnet.WithReusePort(false),
		gnet.WithReuseAddr(true),
		gnet.WithLockOSThread(true),
	)
}

const maxHeader = 16 << 10
const maxBody = 1 << 20

var (
	crlfcrlf = []byte("\r\n\r\n")
	crlf     = []byte("\r\n")

	hdr200 = []byte("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ")
	hdr201 = []byte("HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: ")
	hdr400 = []byte("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: ")
	hdr401 = []byte("HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: ")
	hdr404 = []byte("HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: ")
	hdr500 = []byte("HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: ")
	hdr503 = []byte("HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: ")
)

func (s *httpServer) OnTraffic(c gnet.Conn) gnet.Action {
	in, _ := c.Peek(-1)
	s.out = s.out[:0]
	used, closeConn := 0, false

	for !closeConn {
		r, n := parseRequest(in[used:])
		if n == 0 { // incomplete request: wait for more bytes
			break
		}
		if n < 0 { // malformed request
			s.out = appendResponse(s.out, 400, []byte(`{"error":"bad request"}`), true)
			closeConn = true
			break
		}
		used += n
		closeConn = r.close

		var status int
		status, s.body = serve(r.method, r.path, r.auth, r.body, s.body[:0])
		s.out = appendResponse(s.out, status, s.body, closeConn)
	}

	if used > 0 {
		c.Discard(used)
	}
	if len(s.out) > 0 {
		c.Write(s.out)
	}
	if closeConn {
		return gnet.Close
	}
	return gnet.None
}

func appendResponse(b []byte, status int, body []byte, closeConn bool) []byte {
	var hdr []byte
	switch status {
	case 200:
		hdr = hdr200
	case 201:
		hdr = hdr201
	case 400:
		hdr = hdr400
	case 401:
		hdr = hdr401
	case 404:
		hdr = hdr404
	case 500:
		hdr = hdr500
	case 503:
		hdr = hdr503
	default:
		b = append(b, "HTTP/1.1 "...)
		b = strconv.AppendInt(b, int64(status), 10)
		b = append(b, "\r\nContent-Type: application/json\r\nContent-Length: "...)
		b = appendUint(b, len(body))
		if closeConn {
			b = append(b, "\r\nConnection: close"...)
		}
		b = append(b, "\r\n\r\n"...)
		return append(b, body...)
	}

	b = append(b, hdr...)
	b = appendUint(b, len(body))
	if closeConn {
		b = append(b, "\r\nConnection: close"...)
	}
	b = append(b, "\r\n\r\n"...)
	return append(b, body...)
}

func appendUint(b []byte, n int) []byte {
	if n < 10 {
		return append(b, byte('0'+n))
	}
	if n < 100 {
		return append(b, byte('0'+n/10), byte('0'+n%10))
	}
	if n < 1000 {
		return append(b, byte('0'+n/100), byte('0'+(n/10)%10), byte('0'+n%10))
	}
	if n < 10000 {
		return append(b, byte('0'+n/1000), byte('0'+(n/100)%10), byte('0'+(n/10)%10), byte('0'+n%10))
	}
	return strconv.AppendInt(b, int64(n), 10)
}

type request struct {
	method, path, auth, body []byte
	close                    bool
}

// parseRequest parses one HTTP/1.1 request from b.
// n > 0: bytes consumed; n == 0: incomplete; n < 0: malformed.
func parseRequest(b []byte) (r request, n int) {
	end := bytes.Index(b, crlfcrlf)
	if end < 0 {
		if len(b) > maxHeader {
			return r, -1
		}
		return r, 0
	}
	head := b[:end]
	lineEnd := bytes.Index(head, crlf)
	var line, rest []byte
	if lineEnd < 0 {
		line = head
	} else {
		line = head[:lineEnd]
		rest = head[lineEnd+2:]
	}

	sp1 := bytes.IndexByte(line, ' ')
	if sp1 < 0 {
		return r, -1
	}
	method := line[:sp1]
	line2 := line[sp1+1:]
	sp2 := bytes.IndexByte(line2, ' ')
	if sp2 < 0 {
		return r, -1
	}
	target := line2[:sp2]
	proto := line2[sp2+1:]

	if len(method) == 0 || len(target) == 0 || target[0] != '/' {
		return r, -1
	}
	if q := bytes.IndexByte(target, '?'); q >= 0 {
		target = target[:q]
	}
	r.method, r.path = method, target
	r.close = len(proto) == 8 && string(proto) == "HTTP/1.0"
	clen := 0
	for len(rest) > 0 {
		hEnd := bytes.Index(rest, crlf)
		var h []byte
		if hEnd < 0 {
			h = rest
			rest = nil
		} else {
			h = rest[:hEnd]
			rest = rest[hEnd+2:]
		}
		col := bytes.IndexByte(h, ':')
		if col < 0 {
			return r, -1
		}
		name := h[:col]
		val := bytes.TrimSpace(h[col+1:])
		switch len(name) {
		case 14:
			if bytes.EqualFold(name, []byte("Content-Length")) {
				v := 0
				if len(val) == 0 {
					return r, -1
				}
				for i := 0; i < len(val); i++ {
					c := val[i]
					if c < '0' || c > '9' {
						return r, -1
					}
					v = v*10 + int(c-'0')
					if v > maxBody {
						return r, -1
					}
				}
				clen = v
			}
		case 13:
			if bytes.EqualFold(name, []byte("Authorization")) {
				r.auth = val
			}
		case 10:
			if bytes.EqualFold(name, []byte("Connection")) {
				if bytes.EqualFold(val, []byte("close")) {
					r.close = true
				} else if bytes.EqualFold(val, []byte("keep-alive")) {
					r.close = false
				}
			}
		case 17:
			if bytes.EqualFold(name, []byte("Transfer-Encoding")) {
				return r, -1
			}
		}
	}
	total := end + 4 + clen
	if len(b) < total {
		return r, 0
	}
	r.body = b[end+4 : total]
	return r, total
}
