package main

import (
	"fmt"
	"testing"
)

func TestRenderNumbers(t *testing.T) {
	a := newApp(nil)
	for _, pair := range [][2]int64{
		{0, 9}, {9, 10}, {99, 100}, {999, 1000}, {99999, 100000},
		{9999999, 10000000}, {999999999999999, 1000000000000000},
		{-9223372036854775808, 9223372036854775807},
	} {
		got := a.appendPost(nil, pair[0], nil, nil, nil, pair[1])
		want := fmt.Sprintf(`{"id":%d,"body":"","created_at":"","author":"","like_count":%d}`, pair[0], pair[1])
		if string(got) != want {
			t.Fatalf("integer rendering: %s != %s", got, want)
		}
	}
}
