package main

import (
	"testing"
)

func TestLRUList_Basic(t *testing.T) {
	var l lruList
	l.init()

	if l.head != -1 || l.tail != -1 || l.count != 0 {
		t.Fatalf("expected empty list, got head=%d tail=%d count=%d", l.head, l.tail, l.count)
	}

	// Add mock FDs
	l.add(10, nil)
	l.add(20, nil)
	l.add(30, nil)

	if l.count != 3 {
		t.Fatalf("expected count 3, got %d", l.count)
	}
	if l.head != 10 || l.tail != 30 {
		t.Fatalf("expected head 10 and tail 30, got head=%d tail=%d", l.head, l.tail)
	}

	// Touch middle element (20) -> should move to tail
	l.touch(20)
	if l.head != 10 || l.tail != 20 {
		t.Fatalf("expected head 10 and tail 20 after touch, got head=%d tail=%d", l.head, l.tail)
	}

	// Remove head (10)
	l.remove(10)
	if l.head != 30 || l.tail != 20 || l.count != 2 {
		t.Fatalf("expected head 30, tail 20, count 2, got head=%d tail=%d count=%d", l.head, l.tail, l.count)
	}

	// Pop oldest -> should return 30
	if l.head != 30 {
		t.Fatalf("expected oldest head to be 30, got %d", l.head)
	}
	_ = l.popOldest()
	if l.head != 20 || l.tail != 20 || l.count != 1 {
		t.Fatalf("expected head 20, tail 20, count 1, got head=%d tail=%d count=%d", l.head, l.tail, l.count)
	}

	_ = l.popOldest()
	if l.head != -1 || l.tail != -1 || l.count != 0 {
		t.Fatalf("expected empty list after popping last, got head=%d tail=%d count=%d", l.head, l.tail, l.count)
	}
}

func TestLRUList_ReAdd(t *testing.T) {
	var l lruList
	l.init()

	l.add(5, nil)
	l.add(6, nil)
	l.add(5, nil) // Re-add existing fd -> should move to tail

	if l.count != 2 {
		t.Fatalf("expected count 2, got %d", l.count)
	}
	if l.head != 6 || l.tail != 5 {
		t.Fatalf("expected head 6, tail 5, got head=%d tail=%d", l.head, l.tail)
	}
}
