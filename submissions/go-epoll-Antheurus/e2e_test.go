package main

// End-to-end checks of the HTTP layer that test/test.sh (curl, one request per connection)
// cannot reach: pipelining behind a queued write, split packets, HTTP/1.0, Expect, chunked.
// Run against a server started on a fresh seed copy:
//
//	E2E_ADDR=127.0.0.1:3000 JWT_SECRET=twelve-dollar-challenge go test -run E2E -v

import (
	"bufio"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"strings"
	"testing"
	"time"
)

func e2eAddr(t *testing.T) string {
	addr := os.Getenv("E2E_ADDR")
	if addr == "" {
		t.Skip("E2E_ADDR not set")
	}
	return addr
}

func e2eToken(sub, user string) string {
	enc := base64.RawURLEncoding
	h := enc.EncodeToString([]byte(`{"alg":"HS256","typ":"JWT"}`))
	p := enc.EncodeToString([]byte(fmt.Sprintf(`{"sub":%q,"username":%q,"iat":%d,"exp":%d}`,
		sub, user, time.Now().Unix(), time.Now().Unix()+3600)))
	m := hmac.New(sha256.New, []byte(os.Getenv("JWT_SECRET")))
	m.Write([]byte(h + "." + p))
	return h + "." + p + "." + enc.EncodeToString(m.Sum(nil))
}

func readResponses(t *testing.T, c net.Conn, n int) []*http.Response {
	t.Helper()
	c.SetReadDeadline(time.Now().Add(5 * time.Second))
	br := bufio.NewReader(c)
	var out []*http.Response
	for i := 0; i < n; i++ {
		res, err := http.ReadResponse(br, nil)
		if err != nil {
			t.Fatalf("response %d: %v", i, err)
		}
		body, _ := io.ReadAll(res.Body)
		res.Body = io.NopCloser(strings.NewReader(string(body)))
		res.Header.Set("X-Body", string(body))
		out = append(out, res)
	}
	return out
}

func TestE2EPipelinedWriteKeepsOrder(t *testing.T) {
	addr := e2eAddr(t)
	c, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	tok := e2eToken("777", "e2e_user")
	reqs := "GET /posts/42 HTTP/1.1\r\nHost: x\r\n\r\n" +
		"POST /posts/42/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + tok + "\r\nContent-Length: 0\r\n\r\n" +
		"GET /posts/42 HTTP/1.1\r\nHost: x\r\n\r\n" +
		"POST /posts/42/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + tok + "\r\nContent-Length: 0\r\n\r\n" +
		"GET /health HTTP/1.1\r\nHost: x\r\n\r\n"
	if _, err := c.Write([]byte(reqs)); err != nil {
		t.Fatal(err)
	}
	rs := readResponses(t, c, 5)
	want := []int{200, 201, 200, 200, 200}
	for i, r := range rs {
		if r.StatusCode != want[i] {
			t.Fatalf("response %d: status %d, want %d (%s)", i, r.StatusCode, want[i], r.Header.Get("X-Body"))
		}
	}
	before, after := rs[0].Header.Get("X-Body"), rs[2].Header.Get("X-Body")
	if before == after {
		t.Fatalf("read after a pipelined like did not see it: %s", after)
	}
	if !strings.Contains(rs[3].Header.Get("X-Body"), `"already_liked":true`) || !strings.HasPrefix(rs[4].Header.Get("X-Body"), `{"status":"ok"`) {
		t.Fatalf("out of order: %s / %s", rs[3].Header.Get("X-Body"), rs[4].Header.Get("X-Body"))
	}
}

func TestE2ESplitPacketsAndBody(t *testing.T) {
	addr := e2eAddr(t)
	c, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	body := `{"body":"split across packets"}`
	req := "POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + e2eToken("778", "split_user") +
		"\r\nContent-Type: application/json\r\nContent-Length: " + fmt.Sprint(len(body)) + "\r\n\r\n" + body
	for i := 0; i < len(req); i += 7 {
		c.Write([]byte(req[i:min(i+7, len(req))]))
		time.Sleep(time.Millisecond)
	}
	r := readResponses(t, c, 1)[0]
	if r.StatusCode != 201 || !strings.Contains(r.Header.Get("X-Body"), `"body":"split across packets","created_at":"`) ||
		!strings.Contains(r.Header.Get("X-Body"), `"author":"split_user","like_count":0}}`) {
		t.Fatalf("got %d %s", r.StatusCode, r.Header.Get("X-Body"))
	}
}

func TestE2EConnectionCloseOnWrite(t *testing.T) {
	addr := e2eAddr(t)
	c, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	c.Write([]byte("POST /posts/43/like HTTP/1.1\r\nHost: x\r\nConnection: close\r\nAuthorization: Bearer " +
		e2eToken("779", "close_user") + "\r\nContent-Length: 0\r\n\r\nGET /feed HTTP/1.1\r\nHost: x\r\n\r\n"))
	r := readResponses(t, c, 1)[0]
	if r.StatusCode != 201 || !r.Close {
		t.Fatalf("got %d close=%v", r.StatusCode, r.Close)
	}
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	if n, err := c.Read(make([]byte, 1)); n != 0 || err != io.EOF {
		t.Fatalf("connection stayed open after Connection: close (n=%d err=%v)", n, err)
	}
}

func TestE2EHTTP10AndExpectAndChunked(t *testing.T) {
	addr := e2eAddr(t)
	c, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	c.Write([]byte("GET /health HTTP/1.0\r\n\r\n"))
	r := readResponses(t, c, 1)[0]
	if r.StatusCode != 200 || !r.Close {
		t.Fatalf("HTTP/1.0: %d close=%v", r.StatusCode, r.Close)
	}
	c.Close()

	c, _ = net.Dial("tcp", addr)
	defer c.Close()
	body := `{"body":"expect continue"}`
	c.Write([]byte("POST /posts HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nAuthorization: Bearer " +
		e2eToken("780", "expect_user") + "\r\nContent-Length: " + fmt.Sprint(len(body)) + "\r\n\r\n"))
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	line, _ := bufio.NewReader(c).ReadString('\n')
	if !strings.HasPrefix(line, "HTTP/1.1 100 Continue") {
		t.Fatalf("no 100 Continue: %q", line)
	}
	c.Close()

	c, _ = net.Dial("tcp", addr)
	defer c.Close()
	c.Write([]byte("POST /posts HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"))
	if r := readResponses(t, c, 1)[0]; r.StatusCode != 501 {
		t.Fatalf("chunked: %d", r.StatusCode)
	}
}
