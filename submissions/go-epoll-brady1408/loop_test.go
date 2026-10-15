package main

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"testing"
	"time"
)

// startLoop runs a loop on a random port. budget < 0 keeps the default.
func startLoop(t *testing.T, budget int) *loop {
	t.Helper()
	s, err := newServer(openTestDB(t), testSecret) // closed by the loop's shutdown
	if err != nil {
		t.Fatal(err)
	}
	l, err := newLoop(s, "127.0.0.1", 0)
	if err != nil {
		t.Fatal(err)
	}
	if budget >= 0 {
		l.budget = budget
	}
	go l.run()
	t.Cleanup(func() { l.stop(); <-l.done })
	return l
}

func dial(t *testing.T, l *loop) net.Conn {
	t.Helper()
	c, err := net.Dial("tcp", fmt.Sprintf("127.0.0.1:%d", l.port()))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	c.SetDeadline(time.Now().Add(5 * time.Second))
	return c
}

func readResponse(t *testing.T, r *bufio.Reader) (*http.Response, string) {
	t.Helper()
	resp, err := http.ReadResponse(r, nil)
	if err != nil {
		t.Fatal(err)
	}
	b, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	return resp, string(b)
}

func TestPipelinedRequests(t *testing.T) {
	l := startLoop(t, -1)
	c := dial(t, l)
	c.Write([]byte("GET /posts/1 HTTP/1.1\r\n\r\nGET /nope HTTP/1.1\r\n\r\n"))
	r := bufio.NewReader(c)
	if resp, body := readResponse(t, r); resp.StatusCode != 200 || !strings.Contains(body, `"id":1`) {
		t.Fatalf("%d %s", resp.StatusCode, body)
	}
	if resp, body := readResponse(t, r); resp.StatusCode != 404 || body != `{"error":"not found"}` {
		t.Fatalf("%d %s", resp.StatusCode, body)
	}
}

func TestRequestSplitAcrossWrites(t *testing.T) {
	l := startLoop(t, -1)
	c := dial(t, l)
	tok := token(t, "1")
	payload := `{"body":"split"}`
	c.Write([]byte(fmt.Sprintf("POST /posts HTTP/1.1\r\nAuthorization: Bearer %s\r\nContent-Length: %d\r\n\r\n", tok, len(payload))))
	time.Sleep(100 * time.Millisecond)
	c.Write([]byte(payload))
	resp, body := readResponse(t, bufio.NewReader(c))
	if resp.StatusCode != 201 || !strings.Contains(body, `"body":"split"`) {
		t.Fatalf("%d %s", resp.StatusCode, body)
	}
	// The write is committed: a second connection sees it at the top of the feed.
	c2 := dial(t, l)
	c2.Write([]byte("GET /feed HTTP/1.1\r\n\r\n"))
	if _, body := readResponse(t, bufio.NewReader(c2)); !strings.HasPrefix(body, `{"posts":[{"id":4,"body":"split"`) {
		t.Fatal(body)
	}
}

func TestOverBudgetCloses(t *testing.T) {
	l := startLoop(t, 0) // every connection is over budget
	c := dial(t, l)
	c.Write([]byte("GET /posts/1 HTTP/1.1\r\n\r\n"))
	r := bufio.NewReader(c)
	resp, body := readResponse(t, r)
	if resp.StatusCode != 200 || !resp.Close || !strings.Contains(body, `"id":1`) { // resp.Close reflects "Connection: close"
		t.Fatalf("%v %s", resp.Header, body)
	}
	if _, err := r.ReadByte(); err != io.EOF {
		t.Fatalf("server should close after the response, got %v", err)
	}
}

func TestBadRequestCloses(t *testing.T) {
	l := startLoop(t, -1)
	c := dial(t, l)
	c.Write([]byte("GARBAGE\r\n\r\n"))
	r := bufio.NewReader(c)
	if resp, _ := readResponse(t, r); resp.StatusCode != 400 {
		t.Fatal(resp.StatusCode)
	}
	if _, err := r.ReadByte(); err != io.EOF {
		t.Fatal(err)
	}
}

func TestRewriteHeld(t *testing.T) {
	c := &conn{flags: connOpen | connHeld}
	c.wbuf = appendResponse(nil, 200, []byte(`{"ok":1}`), false)
	c.holdFrom = int32(len(c.wbuf))
	c.wbuf = appendResponse(c.wbuf, 201, []byte(`{"post":{}}`), false)
	rewriteHeld(c)
	out := string(c.wbuf)
	if strings.Count(out, "HTTP/1.1") != 2 || !strings.Contains(out, "HTTP/1.1 200 OK") || !strings.Contains(out, "HTTP/1.1 500 Internal Server Error") || strings.Contains(out, "201") {
		t.Fatal(out)
	}
	if c.flags&connCloseAfter == 0 || c.flags&connHeld != 0 {
		t.Fatalf("flags %b", c.flags)
	}
}
