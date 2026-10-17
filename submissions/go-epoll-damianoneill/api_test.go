package main

import (
	"encoding/binary"
	"strings"
	"testing"
)

func TestParseID(t *testing.T) {
	for in, want := range map[string]int64{"1": 1, "500000": 500000, "999999999": 999999999} {
		if got, ok := parseID([]byte(in)); !ok || got != want {
			t.Errorf("parseID(%q) = %d, %v", in, got, ok)
		}
	}
	for _, in := range []string{"", "0", "-1", "abc", "1.5", "+1", " 1", "99999999999999999999"} {
		if _, ok := parseID([]byte(in)); ok {
			t.Errorf("parseID(%q) accepted", in)
		}
	}
}

func TestParsePostBody(t *testing.T) {
	tests := []struct{ in, wantBody, wantMsg string }{
		{`{"body":"  hi  "}`, "hi", ""},
		{`{"body":"\u00a0x\u3000\ufeff"}`, "x", ""},
		{`{"body":""}`, "", "body is required"},
		{`{"body":"   "}`, "", "body is required"},
		{`{"body":123}`, "", "body is required"},
		{`{}`, "", "body is required"},
		{`[]`, "", "body is required"},
		{`null`, "", "body is required"},
		{`{bad json`, "", "malformed JSON body"},
		{``, "", "malformed JSON body"},
		{`{"body":"` + strings.Repeat("a", 500) + `"}`, strings.Repeat("a", 500), ""},
		{`{"body":"` + strings.Repeat("é", 500) + `"}`, strings.Repeat("é", 500), ""},
		{`{"body":"` + strings.Repeat("a", 501) + `"}`, "", "body must be at most 500 characters"},
	}
	for _, tt := range tests {
		body, msg := parsePostBody([]byte(tt.in))
		if body != tt.wantBody || msg != tt.wantMsg {
			t.Errorf("parsePostBody(%.40q) = (%.20q, %q), want (%.20q, %q)", tt.in, body, msg, tt.wantBody, tt.wantMsg)
		}
	}
}

func TestAppendJSONString(t *testing.T) {
	got := string(appendJSONString(nil, "a\"b\\c\n\r\t\x01 café ✓ <&>/"))
	want := `"a\"b\\c\n\r\t\u0001 café ✓ <&>/"`
	if got != want {
		t.Fatalf("got %s, want %s", got, want)
	}
	if got := string(appendJSONString(nil, "")); got != `""` {
		t.Fatalf("empty: got %s", got)
	}
	if got := string(appendJSONString(nil, "plain")); got != `"plain"` {
		t.Fatalf("plain: got %s", got)
	}
}

func pack(columns ...string) []byte {
	var b []byte
	for _, c := range columns {
		b = binary.NativeEndian.AppendUint32(b, uint32(len(c)))
		b = append(b, c...)
	}
	return b
}

func TestAppendPosts(t *testing.T) {
	rows := append(pack("2", `say "hi"`, "2025-12-31T23:59:19.409Z", "u2", "5"),
		pack("1", "x", "2025-01-01T00:00:34.152Z", "u1", "0")...)
	got, n := appendPosts([]byte("["), rows)
	want := `[{"id":2,"body":"say \"hi\"","created_at":"2025-12-31T23:59:19.409Z","author":"u2","like_count":5},` +
		`{"id":1,"body":"x","created_at":"2025-01-01T00:00:34.152Z","author":"u1","like_count":0}`
	if n != 2 || string(got) != want {
		t.Fatalf("got %d %s", n, got)
	}
	if got, n := appendPosts(nil, nil); n != 0 || len(got) != 0 {
		t.Fatalf("no rows: got %d %q", n, got)
	}
}
