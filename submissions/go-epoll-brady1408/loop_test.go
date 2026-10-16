package main

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"runtime"
	"strings"
	"testing"
	"time"

	"golang.org/x/sys/unix"
)

// startLoop runs a loop on a random port. budget < 0 keeps the default.
func startLoop(t *testing.T, budget int) *loop {
	t.Helper()
	return startLoopWith(t, func(l *loop) {
		if budget >= 0 {
			l.budget = budget
		}
	})
}

// startLoopWith runs a loop on a random port after setup has adjusted it.
func startLoopWith(t *testing.T, setup func(*loop)) *loop {
	t.Helper()
	s, err := newServer(openTestDB(t), testSecret) // closed by the loop's shutdown
	if err != nil {
		t.Fatal(err)
	}
	l, err := newLoop(s, "127.0.0.1", 0)
	if err != nil {
		t.Fatal(err)
	}
	setup(l)
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

// A client that half-closes right after sending still gets its response, and
// a write it made is committed before the socket closes.
func TestHalfCloseStillGetsResponse(t *testing.T) {
	l := startLoop(t, -1)
	c := dial(t, l).(*net.TCPConn)
	tok := token(t, "1")
	payload := `{"body":"halfclose"}`
	c.Write([]byte(fmt.Sprintf("POST /posts HTTP/1.1\r\nAuthorization: Bearer %s\r\nContent-Length: %d\r\n\r\n%s", tok, len(payload), payload)))
	c.CloseWrite()
	r := bufio.NewReader(c)
	resp, body := readResponse(t, r)
	if resp.StatusCode != 201 || !strings.Contains(body, `"body":"halfclose"`) {
		t.Fatalf("%d %s", resp.StatusCode, body)
	}
	if _, err := r.ReadByte(); err != io.EOF {
		t.Fatalf("want EOF after the response, got %v", err)
	}
}

// After a request with Connection: close, nothing pipelined behind it is
// processed (RFC 9112 §9.6): no second response, and no committed write.
func TestNoProcessingAfterConnectionClose(t *testing.T) {
	l := startLoop(t, -1)
	c := dial(t, l)
	tok := token(t, "1")
	// The POST is padded past one read buffer so it is parsed on a later read.
	payload := `{"body":"after-close"` + strings.Repeat(" ", 6000) + `}`
	c.Write([]byte("GET /posts/1 HTTP/1.1\r\nConnection: close\r\n\r\n" +
		fmt.Sprintf("POST /posts HTTP/1.1\r\nAuthorization: Bearer %s\r\nContent-Length: %d\r\n\r\n%s", tok, len(payload), payload)))
	r := bufio.NewReader(c)
	if resp, _ := readResponse(t, r); resp.StatusCode != 200 || !resp.Close {
		t.Fatalf("%d close=%v", resp.StatusCode, resp.Close)
	}
	if _, err := r.ReadByte(); err != io.EOF {
		t.Fatalf("want EOF after Connection: close, got %v", err)
	}
	c2 := dial(t, l)
	c2.Write([]byte("GET /feed HTTP/1.1\r\n\r\n"))
	if _, body := readResponse(t, bufio.NewReader(c2)); strings.Contains(body, "after-close") {
		t.Fatal("a request pipelined after Connection: close was processed and committed")
	}
}

// One socket that pipelines requests without ever reading must not starve
// other connections: the loop caps work per connection per round.
func TestFloodingClientCannotStarveOthers(t *testing.T) {
	l := startLoop(t, -1)
	flood := dial(t, l)
	chunk := bytes.Repeat([]byte("GET /feed HTTP/1.1\r\n\r\n"), 4000) // ~88 KB per write
	go func() {
		for i := 0; i < 200; i++ { // ~17 MB, far more than the loop should accept unread
			if _, err := flood.Write(chunk); err != nil {
				return
			}
		}
	}()
	time.Sleep(300 * time.Millisecond)
	c := dial(t, l)
	t0 := time.Now()
	c.Write([]byte("GET /health HTTP/1.1\r\n\r\n"))
	resp, _ := readResponse(t, bufio.NewReader(c))
	if d := time.Since(t0); resp.StatusCode != 200 || d > 300*time.Millisecond {
		t.Fatalf("health took %v (status %d) while another socket flooded", d, resp.StatusCode)
	}
}

// While a finishing checkpoint runs, writes are parked (their bytes stay in
// the read buffer) and reads are served; releasing the pause answers the write.
func TestWritesParkedDuringCheckpointFinish(t *testing.T) {
	reqs, done := make(chan struct{}, 1), make(chan struct{}, 1)
	l := startLoopWith(t, func(l *loop) {
		// As checkpointTick leaves things while the finishing pass runs.
		l.ckptReq, l.ckptDone = reqs, done
		l.ckptState, l.writesPaused, l.pausedSince = ckptFinish, true, time.Now()
	})
	w := dial(t, l)
	tok := token(t, "1")
	payload := `{"body":"parked"}`
	w.Write([]byte(fmt.Sprintf("POST /posts HTTP/1.1\r\nAuthorization: Bearer %s\r\nContent-Length: %d\r\n\r\n%s", tok, len(payload), payload)))
	rd := dial(t, l)
	rd.Write([]byte("GET /posts/1 HTTP/1.1\r\n\r\n"))
	if resp, _ := readResponse(t, bufio.NewReader(rd)); resp.StatusCode != 200 {
		t.Fatalf("reads must not wait on the pause: %d", resp.StatusCode)
	}
	w.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	if _, err := w.Read(make([]byte, 1)); err == nil {
		t.Fatal("the write was answered while writes were paused")
	}
	w.SetReadDeadline(time.Now().Add(5 * time.Second))
	done <- struct{}{} // the checkpointer reports the finishing pass complete
	l.wake()
	resp, body := readResponse(t, bufio.NewReader(w))
	if resp.StatusCode != 201 || !strings.Contains(body, `"body":"parked"`) {
		t.Fatalf("%d %s", resp.StatusCode, body)
	}
}

// A request that arrives in one packet costs one read: a short read means the
// socket was drained, and the next packet raises a new edge.
func TestOneReadPerRequest(t *testing.T) {
	l := startLoop(t, -1)
	c := dial(t, l)
	c.Write([]byte("GET /posts/1 HTTP/1.1\r\n\r\n"))
	if resp, _ := readResponse(t, bufio.NewReader(c)); resp.StatusCode != 200 {
		t.Fatal(resp.StatusCode)
	}
	l.stop()
	<-l.done
	if l.reads != 1 {
		t.Fatalf("read syscalls for one request: %d, want 1", l.reads)
	}
}

// A request larger than one read buffer keeps being read until it is complete.
func TestRequestLargerThanReadBuffer(t *testing.T) {
	l := startLoop(t, -1)
	c := dial(t, l)
	payload := `{"body":"big","pad":"` + strings.Repeat("x", 3*readBufSize) + `"}`
	c.Write([]byte(fmt.Sprintf("POST /posts HTTP/1.1\r\nAuthorization: Bearer %s\r\nContent-Length: %d\r\n\r\n%s", token(t, "1"), len(payload), payload)))
	resp, body := readResponse(t, bufio.NewReader(c))
	if resp.StatusCode != 201 || !strings.Contains(body, `"body":"big"`) {
		t.Fatalf("%d %s", resp.StatusCode, body)
	}
}

// With one P, a goroutine readied while the loop waits for sockets must get to
// run within a few milliseconds, even with async preemption off: the loop never
// keeps the only P in epoll_wait longer than its wait cap. The body runs in a
// child process so GODEBUG can be set.
func TestLoopYieldsTheOnlyP(t *testing.T) {
	if os.Getenv("TDSC_INNER") == "" {
		cmd := exec.Command(os.Args[0], "-test.run=^TestLoopYieldsTheOnlyP$", "-test.timeout=20s")
		cmd.Env = append(os.Environ(), "TDSC_INNER=1", "GODEBUG=asyncpreemptoff=1")
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("%v\n%s", err, out)
		}
		return
	}
	defer runtime.GOMAXPROCS(runtime.GOMAXPROCS(1))
	startLoop(t, -1)
	for i := 0; i < 5; i++ {
		start := time.Now()
		time.Sleep(time.Millisecond)
		if d := time.Since(start); d > 100*time.Millisecond {
			t.Fatalf("1 ms sleep took %s: the loop is holding the only P", d)
		}
	}
}

// A FIN that lands in the same edge as the request must still be seen, or the
// socket stays open until the idle sweep. TestHalfCloseStillGetsResponse only
// catches this when the FIN happens to arrive before the loop reads.
func TestFinInSameEdgeAsRequest(t *testing.T) {
	s, err := newServer(openTestDB(t), testSecret)
	if err != nil {
		t.Fatal(err)
	}
	l, err := newLoop(s, "127.0.0.1", 0) // never run: the event is delivered by hand
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(l.shutdown)
	fds, err := unix.Socketpair(unix.AF_UNIX, unix.SOCK_STREAM|unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer unix.Close(fds[1])
	c := &conn{fd: int32(fds[0]), flags: connOpen, holdFrom: -1}
	l.conns[fds[0]] = c
	l.open++
	unix.Write(fds[1], []byte("GET /posts/1 HTTP/1.1\r\n\r\n"))
	unix.Shutdown(fds[1], unix.SHUT_WR)
	l.handleEvent(c, unix.EPOLLIN|unix.EPOLLRDHUP)
	if c.flags&connOpen != 0 && c.flags&connCloseAfter == 0 {
		t.Fatal("the FIN delivered with the request was not seen")
	}
}

// A stop-the-world must not be held up by the loop waiting for sockets. The
// scheduler can only interrupt a raw epoll_wait with a signal, so the wait has
// to come back to Go code where the preemption can happen.
func TestStopTheWorldWhileWaiting(t *testing.T) {
	if runtime.GOMAXPROCS(0) < 2 {
		t.Skip("needs a P the loop does not own")
	}
	startLoop(t, -1)
	time.Sleep(20 * time.Millisecond) // the loop is idle in epoll_wait
	for i := 0; i < 20; i++ {
		done := make(chan struct{})
		go func() { runtime.GC(); close(done) }()
		select {
		case <-done:
		case <-time.After(2 * time.Second):
			t.Fatalf("GC %d did not finish in 2 s: stop-the-world is stuck on the loop", i)
		}
	}
}
