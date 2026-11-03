package main

import "testing"

func TestAppendJSONString(t *testing.T) {
	cases := map[string]string{
		"plain":         `"plain"`,
		"":              `""`,
		`say "hi"`:      `"say \"hi\""`,
		`back\slash`:    `"back\\slash"`,
		"tab\there":     `"tab\there"`,
		"nl\n":          `"nl\n"`,
		"cr\r":          `"cr\r"`,
		"\x01":          `"\u0001"`,
		"café ✓ </b>&/": `"café ✓ </b>&/"`, // non-ASCII and <>&/ pass through
	}
	for in, want := range cases {
		if got := string(appendJSONString(nil, []byte(in))); got != want {
			t.Errorf("%q: got %s want %s", in, got, want)
		}
	}
}

func TestAppendPost(t *testing.T) {
	got := string(appendPost(nil, 7, []byte(`a "b"`), []byte("2025-01-01T00:00:34.152Z"), []byte("ann"), 3))
	want := `{"id":7,"body":"a \"b\"","created_at":"2025-01-01T00:00:34.152Z","author":"ann","like_count":3}`
	if got != want {
		t.Fatalf("got %s\nwant %s", got, want)
	}
}

func TestSmallBodies(t *testing.T) {
	if got := string(appendError(nil, "not found")); got != `{"error":"not found"}` {
		t.Fatal(got)
	}
	if got := string(appendLike(nil, true, 12)); got != `{"liked":true,"already_liked":true,"post_id":12}` {
		t.Fatal(got)
	}
	if got := string(appendLike(nil, false, 12)); got != `{"liked":true,"already_liked":false,"post_id":12}` {
		t.Fatal(got)
	}
	if got := string(appendHealthOK(nil, 42)); got != `{"status":"ok","db":"ok","uptime_s":42}` {
		t.Fatal(got)
	}
}
