package main

import (
	"bytes"
	"testing"
)

func TestRenderNumbers(t *testing.T) {
	saved := out.rows[0]
	defer func() { out.rows[0] = saved }()
	a := newApp(nil)
	for _, r := range []row{
		{id: 0, likes: 9},
		{id: 9, likes: 10},
		{id: 99, likes: 100},
		{id: 999, likes: 1000},
		{id: 99999, likes: 100000},
		{id: 9999999, likes: 10000000},
		{id: 999999999999999, likes: 1000000000000000},
		{id: -9223372036854775808, likes: 9223372036854775807},
	} {
		out.rows[0] = r
		buf := make([]byte, 0, 4096)
		n := render(1, buf)
		want := a.appendPost([]byte(`{"post":`), int64(r.id), nil, nil, nil, int64(r.likes))
		want = append(want, '}')
		if n < 0 || !bytes.Equal(buf[:n], want) {
			t.Fatalf("C integer rendering differs: id=%d likes=%d", r.id, r.likes)
		}
	}
}
