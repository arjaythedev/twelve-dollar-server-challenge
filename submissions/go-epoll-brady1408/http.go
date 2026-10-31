package main

import (
	"bytes"
	"strconv"
)

type parseStatus uint8

const (
	parseNeedMore parseStatus = iota
	parseComplete
	parseBad
	parseHeadersTooLarge
	parseBodyTooLarge
)

const (
	methodGET uint8 = iota
	methodPOST
	methodOther
)

const (
	maxHeaderBytes = 8192
	maxBodyBytes   = 16384
)

type request struct {
	method         uint8
	path           []byte
	auth           []byte
	body           []byte
	contentLength  int
	chunked        bool
	connClose      bool
	expectContinue bool
}

var (
	crlfcrlf       = []byte("\r\n\r\n")
	hContentLength = []byte("content-length")
	hAuthorization = []byte("authorization")
	hConnection    = []byte("connection")
	hTransferEnc   = []byte("transfer-encoding")
	hExpect        = []byte("expect")
	vClose         = []byte("close")
	vKeepAlive     = []byte("keep-alive")
	vChunked       = []byte("chunked")
	v100Continue   = []byte("100-continue")
	continue100    = []byte("HTTP/1.1 100 Continue\r\n\r\n")
)

// parseRequest parses one request at the start of buf. Returned slices point
// into buf. n is the number of bytes consumed when st == parseComplete.
func parseRequest(buf []byte) (req request, n int, st parseStatus) {
	end := bytes.Index(buf, crlfcrlf)
	if end < 0 {
		if len(buf) > maxHeaderBytes {
			return req, 0, parseHeadersTooLarge
		}
		return req, 0, parseNeedMore
	}
	if end > maxHeaderBytes {
		return req, 0, parseHeadersTooLarge
	}
	head := buf[:end]
	bodyStart := end + 4

	// Request line: METHOD SP PATH SP HTTP/1.x
	eol := bytes.IndexByte(head, '\r')
	if eol < 0 {
		eol = len(head)
	}
	line := head[:eol]
	sp1 := bytes.IndexByte(line, ' ')
	if sp1 <= 0 {
		return req, 0, parseBad
	}
	rest := line[sp1+1:]
	sp2 := bytes.IndexByte(rest, ' ')
	if sp2 <= 0 {
		return req, 0, parseBad
	}
	version := rest[sp2+1:]
	if len(version) != 8 || string(version[:7]) != "HTTP/1." {
		return req, 0, parseBad
	}
	switch string(line[:sp1]) {
	case "GET":
		req.method = methodGET
	case "POST":
		req.method = methodPOST
	default:
		req.method = methodOther
	}
	req.path = rest[:sp2]
	if q := bytes.IndexByte(req.path, '?'); q >= 0 {
		req.path = req.path[:q]
	}
	req.connClose = version[7] == '0'

	// Headers.
	hdrs := head[eol:]
	for len(hdrs) > 0 {
		if len(hdrs) >= 2 && hdrs[0] == '\r' && hdrs[1] == '\n' {
			hdrs = hdrs[2:]
		}
		if len(hdrs) == 0 {
			break
		}
		le := bytes.IndexByte(hdrs, '\r')
		if le < 0 {
			le = len(hdrs)
		}
		h := hdrs[:le]
		hdrs = hdrs[le:]
		colon := bytes.IndexByte(h, ':')
		if colon <= 0 {
			return req, 0, parseBad
		}
		name := h[:colon]
		val := bytes.TrimSpace(h[colon+1:])
		switch {
		case bytes.EqualFold(name, hContentLength):
			cl, err := strconv.Atoi(string(val))
			if err != nil || cl < 0 {
				return req, 0, parseBad
			}
			req.contentLength = cl
		case bytes.EqualFold(name, hAuthorization):
			req.auth = val
		case bytes.EqualFold(name, hConnection):
			if bytes.EqualFold(val, vClose) {
				req.connClose = true
			} else if bytes.EqualFold(val, vKeepAlive) {
				req.connClose = false
			}
		case bytes.EqualFold(name, hTransferEnc):
			if bytes.EqualFold(val, vChunked) {
				req.chunked = true
			}
		case bytes.EqualFold(name, hExpect):
			if bytes.EqualFold(val, v100Continue) {
				req.expectContinue = true
			}
		}
	}

	if req.chunked {
		return req, bodyStart, parseComplete // caller answers 411 and closes
	}
	if req.contentLength > maxBodyBytes {
		return req, 0, parseBodyTooLarge
	}
	if len(buf) < bodyStart+req.contentLength {
		return req, 0, parseNeedMore
	}
	req.body = buf[bodyStart : bodyStart+req.contentLength]
	return req, bodyStart + req.contentLength, parseComplete
}

var statusLines = map[int]string{
	200: "HTTP/1.1 200 OK\r\n",
	201: "HTTP/1.1 201 Created\r\n",
	400: "HTTP/1.1 400 Bad Request\r\n",
	401: "HTTP/1.1 401 Unauthorized\r\n",
	404: "HTTP/1.1 404 Not Found\r\n",
	411: "HTTP/1.1 411 Length Required\r\n",
	413: "HTTP/1.1 413 Content Too Large\r\n",
	431: "HTTP/1.1 431 Request Header Fields Too Large\r\n",
	500: "HTTP/1.1 500 Internal Server Error\r\n",
	503: "HTTP/1.1 503 Service Unavailable\r\n",
}

// appendResponse appends a complete HTTP/1.1 response with a JSON body.
func appendResponse(dst []byte, status int, body []byte, closeConn bool) []byte {
	dst = append(dst, statusLines[status]...)
	dst = append(dst, "Content-Type: application/json\r\nContent-Length: "...)
	dst = strconv.AppendInt(dst, int64(len(body)), 10)
	if closeConn {
		dst = append(dst, "\r\nConnection: close\r\n\r\n"...)
	} else {
		dst = append(dst, "\r\nConnection: keep-alive\r\n\r\n"...)
	}
	return append(dst, body...)
}
