package main

import (
	"runtime"
	"testing"
	"time"
)

// More than a deferred-work budget must complete across repeated nonwaiting polls.
func TestPollBurstProgress(t *testing.T) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	u, err := newUring(256, 512)
	if err != nil {
		t.Fatal(err)
	}
	defer u.close()
	for i := 0; i < 256; i++ {
		u.next().userData = uint64(i + 1)
	}
	seen := make(map[uint64]bool)
	deadline := time.Now().Add(2 * time.Second)
	for len(seen) < 256 && time.Now().Before(deadline) {
		u.poll()
		if u.err != nil {
			t.Fatal(u.err)
		}
		head, tail := *u.cqHead, atomicLoad(u.cqTail)
		for head != tail {
			c := u.cqes[head&u.cqMask]
			if c.res != 0 || c.userData < 1 || c.userData > 256 || seen[c.userData] {
				t.Fatalf("invalid or duplicate completion: %+v", c)
			}
			seen[c.userData] = true
			head++
		}
		atomicStore(u.cqHead, head)
	}
	if len(seen) != 256 || u.pending != 0 {
		t.Fatalf("incomplete burst: %d pending=%d", len(seen), u.pending)
	}
	u.poll() // Empty polls must also return without requiring a completion.
	if u.err != nil {
		t.Fatal(u.err)
	}
}
