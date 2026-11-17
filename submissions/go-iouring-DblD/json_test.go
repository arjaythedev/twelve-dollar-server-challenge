//go:build linux

package main

import (
	"bytes"
	"encoding/json"
	"math/rand"
	"testing"
)

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
	}
	for _, s := range []string{"", "plain", "héllo ✓ 😀", "tab\there", `quote " and \ slash`, "line\nbreak", "\x00\x1f", "12345678\"", "1234567\"8", "ends with backslash\\"} {
		var back string
		if err := json.Unmarshal(appendString(nil, []byte(s)), &back); err != nil || back != s {
			t.Errorf("%q came back as %q (%v)", s, back, err)
		}
	}
}
