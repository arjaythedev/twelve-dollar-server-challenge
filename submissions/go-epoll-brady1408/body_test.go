package main

import (
	"strings"
	"testing"
)

func TestValidateBody(t *testing.T) {
	cases := []struct{ in, body, err string }{
		{`{"body":"hello"}`, "hello", ""},
		{`{"body":"  padded  "}`, "padded", ""},
		{`{"body":""}`, "", "body is required"},
		{`{"body":"   "}`, "", "body is required"},
		{`{"body":123}`, "", "body is required"},
		{`{"body":null}`, "", "body is required"},
		{`{}`, "", "body is required"},
		{``, "", "body is required"},
		{`{bad json`, "", "malformed JSON body"},
		{`[1,2]`, "", "malformed JSON body"},
		{`{"body":"` + strings.Repeat("a", 501) + `"}`, "", "body must be at most 500 characters"},
		{`{"body":"  ` + strings.Repeat("b", 500) + ` "}`, strings.Repeat("b", 500), ""},
		{`{"body":"` + strings.Repeat("é", 500) + `"}`, strings.Repeat("é", 500), ""}, // runes, not bytes
	}
	for _, c := range cases {
		body, err := validateBody([]byte(c.in))
		if err != c.err || body != c.body {
			t.Errorf("%.40q: got (%q, %q) want (%q, %q)", c.in, body, err, c.body, c.err)
		}
	}
}

func TestParsePostID(t *testing.T) {
	good := map[string]int64{"1": 1, "500000": 500000, "007": 7, "9223372036854775807": 9223372036854775807}
	for in, want := range good {
		if got, ok := parsePostID([]byte(in)); !ok || got != want {
			t.Errorf("%q: got %d %v", in, got, ok)
		}
	}
	for _, in := range []string{"", "0", "-1", "abc", "1.5", "+1", " 1", "1 ", "99999999999999999999", "1e3", "0x10"} {
		if _, ok := parsePostID([]byte(in)); ok {
			t.Errorf("%q should be invalid", in)
		}
	}
}
