package main

import "testing"

func TestEnterRetainsUnconsumedSubmissions(t *testing.T) {
	head, flags := uint32(7), uint32(0)
	r := uring{sqHead: &head, sqFlags: &flags, tail: 9}
	// Even with no cached pending count, kernel-unconsumed entries require enter.
	if !r.needsEnter() {
		t.Fatal("partial submission was skipped")
	}
	head = 9
	if r.needsEnter() {
		t.Fatal("idle ring requires an unnecessary enter")
	}
	for _, flag := range []uint32{sqTaskrun, sqCQOverflow, sqTaskrun | sqCQOverflow} {
		flags = flag
		if !r.needsEnter() {
			t.Fatalf("kernel work ignored: flags=%x", flag)
		}
	}
}
