package main

import (
	"fmt"
	"strings"
	"testing"
	"time"
)

func testServer(t *testing.T) *server {
	t.Helper()
	s, err := newServer(openTestDB(t), testSecret)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(s.close)
	return s
}

// do runs one raw request through parse + handle and returns status line and body.
func do(t *testing.T, s *server, raw string) (status string, body string, held bool) {
	t.Helper()
	req, _, st := parseRequest([]byte(raw))
	if st != parseComplete {
		t.Fatalf("parse %q: %v", raw, st)
	}
	res, held := s.handle(&req, nil, false, time.Now().Unix())
	i := strings.Index(string(res), "\r\n\r\n")
	return strings.SplitN(string(res), "\r\n", 2)[0], string(res[i+4:]), held
}

func token(t *testing.T, sub string) string {
	now := time.Now().Unix()
	return signed(`{"alg":"HS256","typ":"JWT"}`, fmt.Sprintf(`{"sub":%q,"username":"golden_ember_1","iat":%d,"exp":%d}`, sub, now, now+3600))
}

func TestFeedAndPost(t *testing.T) {
	s := testServer(t)
	status, body, held := do(t, s, "GET /feed HTTP/1.1\r\n\r\n")
	want := `{"posts":[{"id":3,"body":"third \"post\"","created_at":"2025-01-01T00:00:03.000Z","author":"golden_ember_1","like_count":0},` +
		`{"id":2,"body":"second post","created_at":"2025-01-01T00:00:02.000Z","author":"golden_ember_1","like_count":0},` +
		`{"id":1,"body":"first post","created_at":"2025-01-01T00:00:01.000Z","author":"golden_ember_1","like_count":1}]}`
	if status != "HTTP/1.1 200 OK" || body != want || held {
		t.Fatalf("%s %s held=%v", status, body, held)
	}
	if status, body, _ := do(t, s, "GET /posts/1 HTTP/1.1\r\n\r\n"); body != `{"post":{"id":1,"body":"first post","created_at":"2025-01-01T00:00:01.000Z","author":"golden_ember_1","like_count":1}}` {
		t.Fatalf("%s %s", status, body)
	}
	if status, body, _ := do(t, s, "GET /posts/999 HTTP/1.1\r\n\r\n"); status != "HTTP/1.1 404 Not Found" || body != `{"error":"post not found"}` {
		t.Fatalf("%s %s", status, body)
	}
	for _, id := range []string{"0", "-1", "abc", "1.5"} {
		if status, body, _ := do(t, s, "GET /posts/"+id+" HTTP/1.1\r\n\r\n"); status != "HTTP/1.1 400 Bad Request" || body != `{"error":"invalid post id"}` {
			t.Fatalf("%s: %s %s", id, status, body)
		}
	}
	if status, body, _ := do(t, s, "GET /nope HTTP/1.1\r\n\r\n"); status != "HTTP/1.1 404 Not Found" || body != `{"error":"not found"}` {
		t.Fatalf("%s %s", status, body)
	}
	if status, body, _ := do(t, s, "GET /health HTTP/1.1\r\n\r\n"); status != "HTTP/1.1 200 OK" || !strings.HasPrefix(body, `{"status":"ok","db":"ok","uptime_s":`) {
		t.Fatalf("%s %s", status, body)
	}
}

func TestAuthBeforeID(t *testing.T) {
	s := testServer(t)
	if status, body, _ := do(t, s, "POST /posts/abc/like HTTP/1.1\r\n\r\n"); status != "HTTP/1.1 401 Unauthorized" || body != `{"error":"missing bearer token"}` {
		t.Fatalf("%s %s", status, body)
	}
	a := "Authorization: Bearer " + token(t, "1") + "\r\n"
	if status, body, _ := do(t, s, "POST /posts/abc/like HTTP/1.1\r\n"+a+"\r\n"); status != "HTTP/1.1 400 Bad Request" || body != `{"error":"invalid post id"}` {
		t.Fatalf("%s %s", status, body)
	}
	if _, body, _ := do(t, s, "POST /posts HTTP/1.1\r\nAuthorization: Bearer abc\r\nContent-Length: 12\r\n\r\n{\"body\":\"x\"}"); body != `{"error":"invalid or expired token"}` {
		t.Fatal(body)
	}
}

func TestCreateAndLike(t *testing.T) {
	s := testServer(t)
	a := "Authorization: Bearer " + token(t, "1") + "\r\n"
	post := func(path, payload string) (string, string, bool) {
		return do(t, s, fmt.Sprintf("POST %s HTTP/1.1\r\n%sContent-Length: %d\r\n\r\n%s", path, a, len(payload), payload))
	}
	for payload, want := range map[string]string{`{"body":""}`: "body is required", `{"body":123}`: "body is required", `{}`: "body is required", `{bad`: "malformed JSON body", `{"body":"` + strings.Repeat("a", 501) + `"}`: "body must be at most 500 characters"} {
		if status, body, _ := post("/posts", payload); status != "HTTP/1.1 400 Bad Request" || body != `{"error":"`+want+`"}` {
			t.Fatalf("%s: %s %s", payload, status, body)
		}
	}
	status, body, held := post("/posts", `{"body":"  hello  "}`)
	if status != "HTTP/1.1 201 Created" || !held || !s.inTx {
		t.Fatalf("%s %s held=%v inTx=%v", status, body, held, s.inTx)
	}
	if !strings.HasPrefix(body, `{"post":{"id":4,"body":"hello","created_at":"`) || !strings.HasSuffix(body, `","author":"golden_ember_1","like_count":0}}`) {
		t.Fatal(body)
	}
	if err := s.commitBatch(); err != nil {
		t.Fatal(err)
	}
	if _, body, _ := do(t, s, "GET /feed HTTP/1.1\r\n\r\n"); !strings.HasPrefix(body, `{"posts":[{"id":4,"body":"hello",`) {
		t.Fatal(body)
	}
	if status, body, held := post("/posts/4/like", ""); status != "HTTP/1.1 201 Created" || body != `{"liked":true,"already_liked":false,"post_id":4}` || !held {
		t.Fatalf("%s %s held=%v", status, body, held)
	}
	if status, body, held := post("/posts/4/like", ""); status != "HTTP/1.1 200 OK" || body != `{"liked":true,"already_liked":true,"post_id":4}` || held {
		t.Fatalf("%s %s held=%v", status, body, held)
	}
	if status, body, held := post("/posts/999/like", ""); status != "HTTP/1.1 404 Not Found" || body != `{"error":"post not found"}` || held {
		t.Fatalf("%s %s held=%v", status, body, held)
	}
	if err := s.commitBatch(); err != nil {
		t.Fatal(err)
	}
	if _, body, _ := do(t, s, "GET /posts/4 HTTP/1.1\r\n\r\n"); !strings.HasSuffix(body, `"like_count":1}}`) {
		t.Fatal(body)
	}
}

func TestTransportErrors(t *testing.T) {
	s := testServer(t)
	req := request{method: methodPOST, path: []byte("/posts"), chunked: true}
	res, _ := s.handle(&req, nil, true, 0)
	if !strings.HasPrefix(string(res), "HTTP/1.1 411 ") || !strings.Contains(string(res), "Connection: close") {
		t.Fatal(string(res))
	}
}

// The feed and the post endpoint must encode a post identically, whatever is in it.
func TestFeedEncodesLikePostEndpoint(t *testing.T) {
	s := testServer(t)
	body := "quote \" backslash \\ slash / apostrophe ' tab \t nl \n cr \r bs \b ff \f ctl \x01\x1f del \x7f ünïcödé 日本 emoji 😀 <&>"
	if err := s.db.Exec(`INSERT INTO posts (id, user_id, body, created_at) VALUES (4, 2, '` + strings.ReplaceAll(body, "'", "''") + `', '2025-01-01T00:00:04.000Z')`); err != nil {
		t.Fatal(err)
	}
	_, feed, _ := do(t, s, "GET /feed HTTP/1.1\r\n\r\n")
	_, post, _ := do(t, s, "GET /posts/4 HTTP/1.1\r\n\r\n")
	wantObj := strings.TrimSuffix(strings.TrimPrefix(post, `{"post":`), `}`)
	if !strings.HasPrefix(feed, `{"posts":[`+wantObj+`,`) {
		t.Fatalf("feed starts %q\nwant object %q", feed, wantObj)
	}
	if !strings.Contains(post, `"author":"other_user"`) || !strings.Contains(post, "\\u0001\\u001f del \x7f") {
		t.Fatalf("post encoding changed: %q", post)
	}
}
