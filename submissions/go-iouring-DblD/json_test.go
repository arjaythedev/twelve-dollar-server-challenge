//go:build linux

package main

import (
	"bytes"
	"encoding/json"
	"math"
	"math/rand"
	"strconv"
	"testing"
)

// The C integer writer (db.c, used for ids and like counts) must agree with strconv byte for
// byte: at every change in the number of digits, at the extremes, for both signs and on
// random values of every length; and it must refuse a buffer it has not been given room in.
func TestJSONInt(t *testing.T) {
	buf := make([]byte, 64)
	check := func(v int64) {
		t.Helper()
		want := strconv.AppendInt(nil, v, 10)
		if m := dbJSONInt(buf, v); m < 0 || !bytes.Equal(buf[:m], want) {
			t.Fatalf("%d: got %q", v, buf[:max(m, 0)])
		}
	}
	for p := uint64(1); ; p *= 10 {
		for _, d := range []int64{-2, -1, 0, 1, 2} {
			if v := int64(p) + d; p <= math.MaxInt64 {
				check(v)
				check(-v)
			}
		}
		if p > math.MaxInt64/10 {
			break
		}
	}
	for _, v := range []int64{0, 9, 10, 99, 100, 1<<32 - 1, 1 << 32, 1<<32 + 1, math.MaxInt64, math.MinInt64, math.MinInt64 + 1} {
		check(v)
		check(-v)
	}
	r := rand.New(rand.NewSource(2))
	for n := 0; n < 2000000; n++ {
		v := int64(r.Uint64() >> uint(r.Intn(64)))
		check(v)
		check(-v)
	}
	// The writer asks for room for the longest number (a sign and 20 digits, rounded to 24)
	// whatever the value: with less it must write nothing.
	for c := 0; c < 24; c++ {
		if m := dbJSONInt(buf[:c], 7); m != -1 {
			t.Fatalf("wrote %d bytes into %d", m, c)
		}
	}
}

// appendString must produce a JSON string that decodes back to the input, for any bytes that
// are valid UTF-8, and must escape exactly what the byte-by-byte rule says.
func TestAppendString(t *testing.T) {
	slow := func(s []byte) []byte {
		b := []byte{'"'}
		for _, c := range s {
			switch {
			case c == '"' || c == '\\':
				b = append(b, '\\', c)
			case c == '\n':
				b = append(b, '\\', 'n')
			case c == '\r':
				b = append(b, '\\', 'r')
			case c == '\t':
				b = append(b, '\\', 't')
			case c < 0x20:
				b = append(b, '\\', 'u', '0', '0', hexDigits[c>>4], hexDigits[c&0xf])
			default:
				b = append(b, c)
			}
		}
		return append(b, '"')
	}
	r := rand.New(rand.NewSource(1))
	cbuf := make([]byte, 512)
	alphabet := []byte("abc xyz\"\\\n\r\t\x00\x01\x1f\x20\x7f\xc3\xa9\xe2\x9c\x93\xf0\x9f\x98\x80/<>&'")
	for n := 0; n < 200000; n++ {
		s := make([]byte, r.Intn(40))
		for i := range s {
			if r.Intn(4) == 0 {
				s[i] = byte(r.Intn(256))
			} else {
				s[i] = alphabet[r.Intn(len(alphabet))]
			}
		}
		got, want := appendString(nil, s), slow(s)
		if !bytes.Equal(got, want) {
			t.Fatalf("%q:\n got %q\nwant %q", s, got, want)
		}
		// The C escaper (db.c, used for the feed and single posts) must agree byte for byte.
		if m := dbJSONString(cbuf, s); m < 0 || !bytes.Equal(cbuf[:m], want) {
			t.Fatalf("C escaper, %q:\n got %q\nwant %q", s, cbuf[:max(m, 0)], want)
		}
		// ...and must refuse, not overrun, when the output does not fit.
		if m := dbJSONString(cbuf[:len(want)-1], s); m != -1 {
			t.Fatalf("C escaper wrote %d bytes of %q into %d", m, s, len(want)-1)
		}
	}
	// Mostly clean strings of every length up to 200, with none, one or two bytes to escape
	// at random places: the lengths and positions the block copies have to get right.
	big := make([]byte, 1400)
	for n := 0; n < 300000; n++ {
		s := make([]byte, r.Intn(201))
		for i := range s {
			s[i] = byte(0x20 + r.Intn(0x60))
			if s[i] == '"' || s[i] == '\\' {
				s[i] = 'a'
			}
		}
		for e := r.Intn(3); e > 0 && len(s) > 0; e-- {
			s[r.Intn(len(s))] = alphabet[7+r.Intn(8)]
		}
		want := slow(s)
		if got := appendString(nil, s); !bytes.Equal(got, want) {
			t.Fatalf("%q:\n got %q\nwant %q", s, got, want)
		}
		if m := dbJSONString(big, s); m < 0 || !bytes.Equal(big[:m], want) {
			t.Fatalf("C escaper, %q:\n got %q\nwant %q", s, big[:max(m, 0)], want)
		}
		if m := dbJSONString(big[:len(want)-1], s); m != -1 {
			t.Fatalf("C escaper wrote %d bytes of %q into %d", m, s, len(want)-1)
		}
	}
	for _, s := range []string{"", "plain", "héllo ✓ 😀", "tab\there", `quote " and \ slash`, "line\nbreak", "\x00\x1f", "12345678\"", "1234567\"8", "ends with backslash\\"} {
		var back string
		if err := json.Unmarshal(appendString(nil, []byte(s)), &back); err != nil || back != s {
			t.Errorf("%q came back as %q (%v)", s, back, err)
		}
	}
}
