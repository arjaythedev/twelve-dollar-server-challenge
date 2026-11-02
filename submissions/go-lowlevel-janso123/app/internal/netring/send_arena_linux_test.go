//go:build linux && (amd64 || arm64)

package netring

import (
	"bytes"
	"golang.org/x/sys/unix"
	"os"
	"testing"
	"time"
)

// The real ring test exercises the kernel's ownership of a send buffer through
// its terminal CQE. The caller's Go slice may change immediately after Send.
func TestSendArenaRealRingRetainsBytesUntilCompletion(t *testing.T) {
	r, err := Open(8, 2)
	if err != nil {
		if os.Getenv("REQUIRE_URING") == "1" {
			t.Fatal(err)
		}
		t.Skipf("io_uring unavailable: %v", err)
	}
	defer r.Close()
	fds, err := unix.Socketpair(unix.AF_UNIX, unix.SOCK_STREAM|unix.SOCK_NONBLOCK, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer unix.Close(fds[0])
	defer unix.Close(fds[1])
	want := []byte("arena send survives source mutation")
	source := bytes.Clone(want)
	if _, err := r.Send(fds[0], source, 1234); err != nil {
		t.Fatal(err)
	}
	clear(source)
	if r.Stats().SendArenaHits != 1 || r.Stats().SendFallbackMmaps != 0 {
		t.Fatalf("unexpected send allocation stats: %+v", r.Stats())
	}
	deadline := time.Now().Add(2 * time.Second)
	completed := false
	for time.Now().Before(deadline) && !completed {
		events, err := r.Wait(100 * time.Millisecond)
		if err != nil {
			t.Fatal(err)
		}
		for _, event := range events {
			if event.Tag == 1234 {
				if event.Result != int32(len(want)) {
					t.Fatalf("send result %d", event.Result)
				}
				completed = true
				r.FinishSend(event.Tag)
			}
		}
	}
	if !completed {
		t.Fatal("send CQE did not arrive")
	}
	got := make([]byte, len(want))
	if n, err := unix.Read(fds[1], got); err != nil || n != len(want) || !bytes.Equal(got, want) {
		t.Fatalf("received %q (%d bytes), error %v", got, n, err)
	}
	if _, err := r.Send(fds[0], want, 1235); err != nil {
		t.Fatal(err)
	}
	if r.Stats().SendArenaHits != 2 {
		t.Fatal("completed slot was not reused")
	}
	// Close must retire the submitted send even without FinishSend.
	if _, err := r.Wait(0); err != nil {
		t.Fatal(err)
	}
	if err := r.Close(); err != nil {
		t.Fatal(err)
	}
	if r.sends != nil || r.sendArena != nil {
		t.Fatal("close retained send memory")
	}
}

func TestSendArenaClassesAndReturn(t *testing.T) {
	a, err := newSendArena([5]int{1, 1, 1, 1, 1})
	if err != nil {
		t.Fatal(err)
	}
	defer a.close()
	for i, n := range []int{1, 4097, 8193, 16385, 32769} {
		s, ok := a.acquire(n)
		if !ok || s.class != i || len(s.buf) != 4096<<i {
			t.Fatalf("size %d: slot %+v, ok %v", n, s, ok)
		}
		if _, ok := a.acquire(n); ok {
			t.Fatalf("class %d did not exhaust", i)
		}
		a.release(s)
		again, ok := a.acquire(n)
		if !ok || again.index != s.index {
			t.Fatalf("class %d not returned", i)
		}
		a.release(again)
	}
	if _, ok := a.acquire(65537); ok {
		t.Fatal("oversize acquired arena memory")
	}
}

func TestSendArenaFallbackAndCloseWithInflight(t *testing.T) {
	a, err := newSendArena([5]int{1})
	if err != nil {
		t.Fatal(err)
	}
	r := &Ring{fd: -1, sends: make(map[uint64]sendSlot), sendArena: a}
	if _, err := r.Send(-1, []byte("first"), 11); err != nil {
		t.Fatal(err)
	}
	if _, err := r.Send(-1, []byte("second"), 12); err != nil {
		t.Fatal(err)
	}
	if r.stats.SendArenaHits != 1 || r.stats.SendFallbackMmaps != 1 {
		t.Fatalf("stats: %+v", r.stats)
	}
	if len(r.sends) != 2 {
		t.Fatal("send ownership lost")
	}
	if err := r.Close(); err != nil {
		t.Fatal(err)
	}
	if r.sendArena != nil || r.sends != nil {
		t.Fatal("close retained send memory")
	}
}

func TestSendArenaFallbackFinishesAndReturnsClassSlot(t *testing.T) {
	a, err := newSendArena([5]int{1})
	if err != nil {
		t.Fatal(err)
	}
	defer a.close()
	r := &Ring{fd: -1, sends: make(map[uint64]sendSlot), sendArena: a}
	if _, err := r.Send(-1, []byte("arena"), 21); err != nil {
		t.Fatal(err)
	}
	if _, err := r.Send(-1, []byte("fallback"), 22); err != nil {
		t.Fatal(err)
	}
	if _, err := r.Send(-1, make([]byte, 65537), 23); err != nil {
		t.Fatal(err)
	}
	if r.stats.SendArenaHits != 1 || r.stats.SendFallbackMmaps != 2 || r.stats.SendMappedBytes != 4096+131072 {
		t.Fatalf("unexpected fallback accounting: %+v", r.stats)
	}
	r.FinishSend(22)
	r.FinishSend(23)
	if r.stats.SendMappedBytes != 0 {
		t.Fatalf("fallback mappings retained: %d bytes", r.stats.SendMappedBytes)
	}
	r.FinishSend(21)
	if _, err := r.Send(-1, []byte("reused"), 24); err != nil {
		t.Fatal(err)
	}
	if r.stats.SendArenaHits != 2 || r.stats.SendFallbackMmaps != 2 {
		t.Fatalf("class slot was not reused: %+v", r.stats)
	}
	r.FinishSend(24)
}

func TestSendArenaRejectsDoubleReturn(t *testing.T) {
	a, err := newSendArena([5]int{1})
	if err != nil {
		t.Fatal(err)
	}
	defer a.close()
	s, ok := a.acquire(1)
	if !ok {
		t.Fatal("no slot")
	}
	a.release(s)
	defer func() {
		if recover() == nil {
			t.Fatal("double return accepted")
		}
	}()
	a.release(s)
}
