package main

import (
	"path/filepath"
	"strings"
	"testing"
	"time"
)

const testSecret = "twelve-dollar-challenge"

// newTestServer builds the real handler over a real database: the validation bug below is only a
// bug in combination with what SQLite does with the value.
func newTestServer(t *testing.T) *server {
	t.Helper()
	d, err := openDBCreate(filepath.Join(t.TempDir(), "api_test.db"), trainSchema)
	if err != nil {
		t.Fatalf("open database: %v", err)
	}
	for _, sql := range []string{
		`INSERT INTO users (id, username) VALUES (1, 'user_1')`,
		`INSERT INTO posts (id, user_id, body) VALUES (1, 1, 'a seeded post')`,
	} {
		if err := d.exec(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	return &server{
		db:         d,
		ver:        newVerifier([]byte(testSecret)),
		started:    time.Now(),
		trim:       make([]byte, 0, 4096),
		likeCounts: make([]int32, maxFeedSpan),
	}
}

// do runs one request through the router and returns the status and the body it rendered.
func do(t *testing.T, s *server, method, path, body string) (int, string) {
	t.Helper()
	r := &request{
		method: []byte(method),
		path:   []byte(path),
		auth:   []byte("Bearer " + trainToken(testSecret, 1, "user_1")),
		body:   []byte(body),
	}
	out := make([]byte, hdrReserve+maxRespBytes)
	status, rendered := s.handle(r, out[hdrReserve:hdrReserve:len(out)])
	return status, string(rendered)
}

// "/posts/like" matches both affixes on overlapping bytes. It routed to likePost, which sliced
// path[7:6] and panicked; nothing recovers, so one request took the server down.
func TestLikePathWithNoIDIsNotPanicAndNotFound(t *testing.T) {
	s := newTestServer(t)

	status, body := do(t, s, "POST", "/posts/like", "")
	if status != 404 || body != errNotFound {
		t.Fatalf("POST /posts/like: got %d %s, want 404 %s", status, body, errNotFound)
	}

	// ":id" matches at least one character, so an empty segment is a 404, not a bad id.
	status, body = do(t, s, "POST", "/posts//like", "")
	if status != 404 || body != errNotFound {
		t.Fatalf("POST /posts//like: got %d %s, want 404 %s", status, body, errNotFound)
	}

	// Present but not a number is this route with a bad id.
	status, body = do(t, s, "POST", "/posts/abc/like", "")
	if status != 400 || body != errInvalidID {
		t.Fatalf("POST /posts/abc/like: got %d %s, want 400 %s", status, body, errInvalidID)
	}

	// The route itself still works.
	if status, _ := do(t, s, "POST", "/posts/1/like", ""); status != 201 {
		t.Fatalf("POST /posts/1/like: got %d, want 201", status)
	}
}

// length() stops at the first NUL, so a body starting with one is text posts.body's CHECK can
// never accept. The insert failed it and answered 500, where the spec answers 201 for any
// non-empty body of at most 500 characters.
func TestCreatePostStoresABodyTheSpecAccepts(t *testing.T) {
	s := newTestServer(t)

	// Two characters, non-empty after trimming: a post, and the body comes back as it went in.
	status, body := do(t, s, "POST", "/posts", `{"body":"\u0000x"}`)
	if status != 201 || !strings.Contains(body, `"body":"\u0000x"`) {
		t.Fatalf("NUL-first body: got %d %s, want 201 with the body echoed", status, body)
	}
	if status, got := do(t, s, "GET", "/posts/2", ""); status != 200 ||
		!strings.Contains(got, `"body":"\u0000x"`) {
		t.Fatalf("read back: got %d %s, want 200 with the body echoed", status, got)
	}

	// A NUL anywhere else leaves the value storable as text: 411 characters, so still a post.
	long := `{"body":"aaaaaaaaaa\u0000` + strings.Repeat("b", 400) + `"}`
	if status, body := do(t, s, "POST", "/posts", long); status != 201 {
		t.Fatalf("NUL mid-body: got %d %s, want 201", status, body)
	}

	// The character limit is still the spec's, counted over the whole body.
	over := `{"body":"` + strings.Repeat("c", 501) + `"}`
	if status, body := do(t, s, "POST", "/posts", over); status != 400 || body != errBodyTooLong {
		t.Fatalf("501-character body: got %d %s, want 400 %s", status, body, errBodyTooLong)
	}

	// The one input the fixed schema cannot hold either way, so the one place this still parts
	// company with the spec: 201 characters, which the spec accepts, but a leading NUL puts
	// length() at 0 as text and the 601 bytes past the bound as a blob. The spec's catch-all.
	huge := `{"body":"\u0000` + strings.Repeat("\u20ac", 200) + `"}`
	if status, body := do(t, s, "POST", "/posts", huge); status != 500 || body != errInternal {
		t.Fatalf("NUL-first multibyte body: got %d %s, want 500 %s", status, body, errInternal)
	}
}
