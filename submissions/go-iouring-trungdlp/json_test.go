package main

import (
	"bytes"
	"encoding/json"
	"math/rand"
	"testing"
	"unsafe"
)

func TestAppendEsc(t *testing.T) {
	r := rand.New(rand.NewSource(1))
	alpha := []byte("abc \"\\\x00\x01\x1f\x20\x7f\x80\xc3\xa9/<>&")
	for n := 0; n < 20000; n++ {
		s := make([]byte, r.Intn(40))
		for i := range s {
			if r.Intn(4) == 0 {
				s[i] = alpha[r.Intn(len(alpha))]
			} else {
				s[i] = byte('a' + r.Intn(26))
			}
		}
		got := appendEsc(nil, s)
		want := appendEscSlow(nil, s)
		if !bytes.Equal(got, want) {
			t.Fatalf("%q: got %q want %q", s, got, want)
		}
		// Valid UTF-8 input must decode back to itself.
		var back string
		if json.Valid(s) || true {
			if err := json.Unmarshal(append(append([]byte{'"'}, got...), '"'), &back); err == nil && bytes.Equal([]byte(back), s) == false && bytes.Equal([]byte(string([]rune(string(s)))), s) {
				t.Fatalf("%q round-trips to %q", s, back)
			}
		}
	}
}

func TestEnd(t *testing.T) {
	for _, body := range []string{"", "x", "{\"a\":1}", string(make([]byte, 12345))} {
		b, s := start(nil, 200)
		b = append(b, body...)
		b = end(b, s)
		want := "Content-Length: " + string(bytes.Repeat([]byte{' '}, 5-len(itoa(len(body))))) + itoa(len(body)) + "\r\n\r\n"
		if !bytes.Contains(b, []byte(want)) {
			t.Fatalf("%q", b[:min(len(b), 80)])
		}
	}
}

func itoa(n int) string { return string(appendInt(nil, int64(n))) }

func BenchmarkAppendEsc(b *testing.B) {
	s := bytes.Repeat([]byte("golden_ember_1 says hi at 2026-11-06T17:21:21.207Z (VU 1, iter 2) "), 3)
	buf := make([]byte, 0, 4096)
	b.SetBytes(int64(len(s)))
	for i := 0; i < b.N; i++ {
		buf = appendEsc(buf[:0], s)
	}
}

// td_render (C) must produce exactly what the Go renderer does, and refuse what doesn't fit.
func TestRender(t *testing.T) {
	r := rand.New(rand.NewSource(2))
	alpha := []byte("abc \"\\\x00\x01\x1f\x20\x7f\x80\xc3\xa9/<>&")
	str := func(max int) []byte {
		s := make([]byte, r.Intn(max))
		for i := range s {
			if r.Intn(30) == 0 {
				s[i] = alpha[r.Intn(len(alpha))]
			} else {
				s[i] = byte('a' + r.Intn(26))
			}
		}
		return s
	}
	set32 := func(p *uint32, v int) { *p = uint32(v) }
	for iter := 0; iter < 3000; iter++ {
		n := r.Intn(21)
		*(*int32)(unsafe.Pointer(&out.nrows)) = int32(n)
		off := 0
		var want []byte
		want = append(want, `{"posts":[`...)
		for i := 0; i < n; i++ {
			row := &out.rows[i]
			id, likes := r.Int63n(1<<40), r.Int63n(5000)
			*(*int64)(unsafe.Pointer(&row.id)) = id
			*(*int64)(unsafe.Pointer(&row.likes)) = likes
			var ts [3][]byte
			for k, mx := range []int{600, 30, 40} {
				ts[k] = str(mx)
				copy(outText[off:], ts[k])
				ptrs := [3][2]*uint32{
					{(*uint32)(unsafe.Pointer(&row.body_off)), (*uint32)(unsafe.Pointer(&row.body_len))},
					{(*uint32)(unsafe.Pointer(&row.created_off)), (*uint32)(unsafe.Pointer(&row.created_len))},
					{(*uint32)(unsafe.Pointer(&row.author_off)), (*uint32)(unsafe.Pointer(&row.author_len))},
				}
				set32(ptrs[k][0], off)
				set32(ptrs[k][1], len(ts[k]))
				off += len(ts[k])
			}
			if i > 0 {
				want = append(want, ',')
			}
			want = appendPost(want, id, ts[0], ts[1], ts[2], likes)
		}
		*(*uint32)(unsafe.Pointer(&out.ntext)) = uint32(off)
		want = append(want, "]}"...)
		capa := 512 + r.Intn(16000)
		buf := make([]byte, 3, capa)
		got := render(0, buf)
		if got < 0 {
			if len(want)+200 < capa-3 && !bytes.ContainsAny(want, "\\") {
				t.Fatalf("refused %d bytes with %d free", len(want), capa-3)
			}
			continue
		}
		if !bytes.Equal(buf[3:3+got], want) {
			t.Fatalf("got  %q\nwant %q", buf[3:3+got], want)
		}
	}
}
