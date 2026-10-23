package main

import (
	"bytes"
	"strings"
	"testing"
	"time"
)

const meminfoFixture = `MemTotal:        2010112 kB
MemFree:          123456 kB
MemAvailable:    1173504 kB
Buffers:           12345 kB
Cached:           987654 kB
SwapTotal:             0 kB
`

func TestParseMemAvailableKiB(t *testing.T) {
	cases := []struct {
		name string
		in   string
		want uint64
		ok   bool
	}{
		{"full meminfo", meminfoFixture, 1173504, true},
		{"only field", "MemAvailable:    262144 kB\n", 262144, true},
		{"no trailing newline", "MemAvailable: 42 kB", 42, true},
		{"zero", "MemAvailable: 0 kB\n", 0, true},
		{"absent", "MemTotal: 2010112 kB\nMemFree: 1 kB\n", 0, false},
		{"malformed value", "MemAvailable: abc kB\n", 0, false},
		{"empty value", "MemAvailable:\n", 0, false},
		{"empty input", "", 0, false},
		{"prefix lookalike ignored", "MemAvailableX: 7 kB\nMemAvailable: 9 kB\n", 9, true},
	}
	for _, c := range cases {
		got, ok := parseMemAvailableKiB([]byte(c.in))
		if got != c.want || ok != c.ok {
			t.Errorf("%s: got (%d,%v), want (%d,%v)", c.name, got, ok, c.want, c.ok)
		}
	}
}

func TestHysteresis(t *testing.T) {
	h := hysteresis{lowKiB: 256 * 1024, highKiB: 384 * 1024}
	if h.update(1024 * 1024) {
		t.Fatal("healthy memory must not engage")
	}
	if !h.update(256*1024 - 1) {
		t.Fatal("strictly below low must engage")
	}
	if !h.update(256 * 1024) {
		t.Fatal("at low while engaged must stay engaged (release requires >= high)")
	}
	if !h.update(384*1024 - 1) {
		t.Fatal("between low and high must stay engaged")
	}
	if h.update(384 * 1024) {
		t.Fatal("at high must release")
	}
	if h.update(256 * 1024) {
		t.Fatal("at low while released must not engage (engage requires < low)")
	}
	if !h.update(0) {
		t.Fatal("zero available must engage")
	}
}

func TestHysteresisDisabled(t *testing.T) {
	h := hysteresis{lowKiB: 0, highKiB: 0}
	for _, avail := range []uint64{0, 1, 100 * 1024} {
		if h.update(avail) {
			t.Fatalf("low=0 must disable the memory watermark, engaged at %d", avail)
		}
	}
}

// TestShedHysteresis pins the emergency watermark's two thresholds: it engages
// strictly below shed and releases at (or above) low, so a shed episode cannot
// flap while the guard is still only "engaged".
func TestShedHysteresis(t *testing.T) {
	h := hysteresis{lowKiB: 384 * 1024, highKiB: 640 * 1024}
	if h.update(640 * 1024) {
		t.Fatal("at low while released must not engage")
	}
	if !h.update(384*1024 - 1) {
		t.Fatal("strictly below shed must engage")
	}
	if !h.update(384 * 1024) {
		t.Fatal("at shed while engaged must stay engaged (release requires >= low)")
	}
	if !h.update(640*1024 - 1) {
		t.Fatal("between shed and low must stay engaged")
	}
	if h.update(640 * 1024) {
		t.Fatal("at low must release")
	}
	if h.update(384 * 1024) {
		t.Fatal("between shed and low while released must not engage")
	}
	if !h.update(0) {
		t.Fatal("zero available must engage")
	}
}

func newTestGuard(lowMiB, highMiB, shedMiB uint64, maxConns int64) (*memGuard, *bytes.Buffer) {
	buf := &bytes.Buffer{}
	g := &memGuard{
		lowMiB: lowMiB, highMiB: highMiB, shedMiB: shedMiB, maxConns: maxConns, out: buf,
		hys:  hysteresis{lowKiB: lowMiB * 1024, highKiB: highMiB * 1024},
		shed: hysteresis{lowKiB: shedMiB * 1024, highKiB: lowMiB * 1024},
		// Startup grace already elapsed, so the fail-closed path does not mask
		// the transitions under test.
		start: time.Now().Add(-time.Hour),
	}
	return g, buf
}

// setSample publishes an immutable sample as if the sampler had taken it at
// `at` (monotonic).
func setSample(g *memGuard, availKiB uint64, at time.Time) {
	g.sample.Store(&memSample{availKiB: availKiB, monoNanos: at.Sub(processStart).Nanoseconds()})
}

func TestMemGuardUpdateMemoryTransitions(t *testing.T) {
	g, buf := newTestGuard(16, 24, 0, 0) // shedding disabled
	now := time.Now()
	setSample(g, 100*1024, now)
	if g.update(0, now) {
		t.Fatal("plenty of memory must not block")
	}
	setSample(g, 15*1024, now.Add(time.Millisecond))
	if !g.update(3, now.Add(time.Millisecond)) {
		t.Fatal("below low must block")
	}
	if !g.update(3, now.Add(2*time.Millisecond)) {
		t.Fatal("must stay blocked below high")
	}
	setSample(g, 20*1024, now.Add(3*time.Millisecond))
	if !g.update(3, now.Add(3*time.Millisecond)) {
		t.Fatal("between watermarks must stay blocked")
	}
	setSample(g, 24*1024, now.Add(4*time.Millisecond))
	if g.update(3, now.Add(4*time.Millisecond)) {
		t.Fatal("at high must release")
	}
	out := buf.String()
	if n := strings.Count(out, "memguard engaged:"); n != 1 {
		t.Fatalf("want exactly 1 engaged line, got %d: %q", n, out)
	}
	if n := strings.Count(out, "memguard released:"); n != 1 {
		t.Fatalf("want exactly 1 released line, got %d: %q", n, out)
	}
	if !strings.Contains(out, "mem_available=15MiB") || !strings.Contains(out, "low=16MiB") || !strings.Contains(out, "high=24MiB") {
		t.Fatalf("engaged line missing fields: %q", out)
	}
}

// TestMemGuardShedTransitions checks the third watermark: below shed the guard
// stays engaged but additionally enters shed mode, and it leaves shed at low
// without leaving "engaged" (which requires high).
func TestMemGuardShedTransitions(t *testing.T) {
	g, buf := newTestGuard(16, 24, 8, 0)
	now := time.Now()
	setSample(g, 100*1024, now)
	if g.update(0, now) || g.shedding() {
		t.Fatal("plenty of memory must neither block nor shed")
	}
	setSample(g, 7*1024, now.Add(time.Millisecond)) // < shed (8)
	if !g.update(0, now.Add(time.Millisecond)) {
		t.Fatal("below shed must block")
	}
	if !g.shedding() {
		t.Fatal("below shed must shed")
	}
	setSample(g, 12*1024, now.Add(2*time.Millisecond)) // shed < 12 < low (16)
	if !g.update(0, now.Add(2*time.Millisecond)) || !g.shedding() {
		t.Fatal("between shed and low must stay blocked and shedding")
	}
	setSample(g, 16*1024, now.Add(3*time.Millisecond)) // == low
	if !g.update(0, now.Add(3*time.Millisecond)) {
		t.Fatal("at low must stay blocked (release requires high)")
	}
	if g.shedding() {
		t.Fatal("at low must leave shed mode")
	}
	out := buf.String()
	if n := strings.Count(out, "memguard shed engaged:"); n != 1 {
		t.Fatalf("want exactly 1 shed engaged line, got %d: %q", n, out)
	}
	if n := strings.Count(out, "memguard shed released:"); n != 1 {
		t.Fatalf("want exactly 1 shed released line, got %d: %q", n, out)
	}
	if !strings.Contains(out, "closed_idle=") || !strings.Contains(out, "closed_after_response=") {
		t.Fatalf("shed release line missing counts: %q", out)
	}
}

func TestMemGuardUpdateConnectionCap(t *testing.T) {
	g, buf := newTestGuard(0, 0, 0, 50) // memory watermark and shedding disabled
	now := time.Now()
	setSample(g, 1, now)
	if g.update(49, now) {
		t.Fatal("49 of 50 must not block")
	}
	if !g.update(50, now.Add(time.Millisecond)) {
		t.Fatal("at cap must block")
	}
	if !g.update(120, now.Add(2*time.Millisecond)) {
		t.Fatal("above cap must block")
	}
	if g.update(49, now.Add(3*time.Millisecond)) {
		t.Fatal("below cap must release")
	}
	out := buf.String()
	if n := strings.Count(out, "memguard engaged:"); n != 1 {
		t.Fatalf("want exactly 1 engaged line, got %d: %q", n, out)
	}
	if n := strings.Count(out, "memguard released:"); n != 1 {
		t.Fatalf("want exactly 1 released line, got %d: %q", n, out)
	}
	if !strings.Contains(out, "max_conns=50") {
		t.Fatalf("lines must report the cap: %q", out)
	}
}

func TestMemGuardUpdateLogRateLimit(t *testing.T) {
	g, buf := newTestGuard(16, 24, 0, 0)
	now := time.Now()
	setSample(g, 15*1024, now)
	if !g.update(0, now) {
		t.Fatal("must engage")
	}
	// Fast flapping across the release threshold: the second engage inside the
	// rate-limit window must not produce another line, but state still tracks.
	setSample(g, 24*1024, now.Add(100*time.Millisecond))
	if g.update(0, now.Add(100*time.Millisecond)) {
		t.Fatal("must release")
	}
	setSample(g, 15*1024, now.Add(200*time.Millisecond))
	if !g.update(0, now.Add(200*time.Millisecond)) {
		t.Fatal("must re-engage")
	}
	setSample(g, 24*1024, now.Add(300*time.Millisecond))
	if g.update(0, now.Add(300*time.Millisecond)) {
		t.Fatal("must release again")
	}
	setSample(g, 15*1024, now.Add(1100*time.Millisecond))
	if !g.update(0, now.Add(1100*time.Millisecond)) {
		t.Fatal("must re-engage after window")
	}
	out := buf.String()
	if n := strings.Count(out, "memguard engaged:"); n != 2 {
		t.Fatalf("want 2 engaged lines (one suppressed), got %d: %q", n, out)
	}
	if n := strings.Count(out, "memguard released:"); n != 1 {
		t.Fatalf("want 1 released line (one suppressed), got %d: %q", n, out)
	}
}

func TestMemGuardCapHelpers(t *testing.T) {
	var nilGuard *memGuard
	if nilGuard.capReached(1<<30) || nilGuard.overCap(1<<30) {
		t.Fatal("nil guard must never cap")
	}
	g, _ := newTestGuard(0, 0, 0, 50)
	if g.capReached(49) || !g.capReached(50) || !g.capReached(51) {
		t.Fatal("capReached must be open >= cap")
	}
	if g.overCap(49) || g.overCap(50) || !g.overCap(51) {
		t.Fatal("overCap must be open > cap")
	}
	unlimited, _ := newTestGuard(0, 0, 0, 0)
	if unlimited.capReached(1<<30) || unlimited.overCap(1<<30) {
		t.Fatal("maxConns=0 must be unlimited")
	}
}

func TestMemGuardConfig(t *testing.T) {
	t.Setenv("MEMGUARD_LOW_MIB", "")
	t.Setenv("MEMGUARD_HIGH_MIB", "")
	t.Setenv("MEMGUARD_SHED_MIB", "")
	t.Setenv("MAX_CONNECTIONS", "")
	low, high, shed, maxConns, e := memGuardConfig()
	if e != nil || low != defaultMemGuardLowMiB || high != defaultMemGuardHighMiB || shed != defaultMemGuardShedMiB || maxConns != 0 {
		t.Fatalf("defaults: got (%d,%d,%d,%d,%v), want (%d,%d,%d,0,nil)",
			low, high, shed, maxConns, e, defaultMemGuardLowMiB, defaultMemGuardHighMiB, defaultMemGuardShedMiB)
	}
	t.Setenv("MEMGUARD_LOW_MIB", "8")
	t.Setenv("MEMGUARD_HIGH_MIB", "16")
	t.Setenv("MEMGUARD_SHED_MIB", "4")
	t.Setenv("MAX_CONNECTIONS", "50")
	low, high, shed, maxConns, e = memGuardConfig()
	if e != nil || low != 8 || high != 16 || shed != 4 || maxConns != 50 {
		t.Fatalf("overrides: got (%d,%d,%d,%d,%v), want (8,16,4,50,nil)", low, high, shed, maxConns, e)
	}
	// MAX_CONNECTIONS=0 means unlimited.
	t.Setenv("MAX_CONNECTIONS", "0")
	if _, _, _, maxConns, e = memGuardConfig(); e != nil || maxConns != 0 {
		t.Fatalf("zero cap: got (%d,%v), want (0,nil)", maxConns, e)
	}
	t.Setenv("MAX_CONNECTIONS", "")
	t.Setenv("MEMGUARD_LOW_MIB", "0")
	t.Setenv("MEMGUARD_SHED_MIB", "")
	if _, _, shed, _, e = memGuardConfig(); e != nil || shed != 0 {
		t.Fatalf("low=0 disables the memory watermark and shedding: got shed=%d err=%v", shed, e)
	}
	// Values above the sanity ceiling are rejected (overflow guard).
	for _, bad := range [][2]string{
		{"MEMGUARD_LOW_MIB", "abc"},
		{"MEMGUARD_LOW_MIB", "-4"},
		{"MEMGUARD_LOW_MIB", "1048577"},
		{"MEMGUARD_HIGH_MIB", "-1"},
		{"MEMGUARD_HIGH_MIB", "1048577"},
		{"MEMGUARD_SHED_MIB", "abc"},
		{"MEMGUARD_SHED_MIB", "-1"},
		{"MEMGUARD_SHED_MIB", "1048577"},
		{"MAX_CONNECTIONS", "-1"},
		{"MAX_CONNECTIONS", "x"},
	} {
		t.Setenv("MEMGUARD_LOW_MIB", "16")
		t.Setenv("MEMGUARD_HIGH_MIB", "32")
		t.Setenv("MEMGUARD_SHED_MIB", "8")
		t.Setenv("MAX_CONNECTIONS", "")
		t.Setenv(bad[0], bad[1])
		if _, _, _, _, e = memGuardConfig(); e == nil {
			t.Fatalf("%s=%q must be rejected", bad[0], bad[1])
		}
	}
	// Exactly the ceiling is accepted.
	t.Setenv("MEMGUARD_LOW_MIB", "16")
	t.Setenv("MEMGUARD_HIGH_MIB", "1048576")
	t.Setenv("MEMGUARD_SHED_MIB", "0")
	t.Setenv("MAX_CONNECTIONS", "")
	if _, _, _, _, e = memGuardConfig(); e != nil {
		t.Fatalf("high == 1<<20 must be accepted: %v", e)
	}
	t.Setenv("MEMGUARD_SHED_MIB", "8")
	t.Setenv("MEMGUARD_LOW_MIB", "32")
	t.Setenv("MEMGUARD_HIGH_MIB", "32")
	if _, _, _, _, e = memGuardConfig(); e == nil {
		t.Fatal("high must be strictly greater than low")
	}
}

// TestMemGuardShedConfig pins the SHED < LOW relationship and the two ways to
// disable shedding (SHED=0, or the whole memory watermark via LOW=0).
func TestMemGuardShedConfig(t *testing.T) {
	base := func() {
		t.Setenv("MEMGUARD_LOW_MIB", "16")
		t.Setenv("MEMGUARD_HIGH_MIB", "32")
		t.Setenv("MEMGUARD_SHED_MIB", "")
		t.Setenv("MAX_CONNECTIONS", "")
	}
	base()
	t.Setenv("MEMGUARD_SHED_MIB", "16") // == low
	if _, _, _, _, e := memGuardConfig(); e == nil {
		t.Fatal("SHED == LOW must be rejected")
	}
	base()
	t.Setenv("MEMGUARD_SHED_MIB", "20") // > low
	if _, _, _, _, e := memGuardConfig(); e == nil {
		t.Fatal("SHED > LOW must be rejected")
	}
	base()
	t.Setenv("MEMGUARD_SHED_MIB", "15") // just below low
	if _, _, shed, _, e := memGuardConfig(); e != nil || shed != 15 {
		t.Fatalf("SHED just below LOW must be accepted: shed=%d err=%v", shed, e)
	}
	base()
	t.Setenv("MEMGUARD_SHED_MIB", "0") // disabled
	if _, _, shed, _, e := memGuardConfig(); e != nil || shed != 0 {
		t.Fatalf("SHED=0 must disable shedding: shed=%d err=%v", shed, e)
	}
	base()
	t.Setenv("MEMGUARD_LOW_MIB", "0")
	t.Setenv("MEMGUARD_SHED_MIB", "") // unset with LOW=0 -> silently disabled
	if _, _, shed, _, e := memGuardConfig(); e != nil || shed != 0 {
		t.Fatalf("LOW=0 with unset SHED must disable shedding: shed=%d err=%v", shed, e)
	}
	base()
	t.Setenv("MEMGUARD_LOW_MIB", "0")
	t.Setenv("MEMGUARD_SHED_MIB", "5") // explicit while disabled -> error
	if _, _, _, _, e := memGuardConfig(); e == nil {
		t.Fatal("SHED > 0 with LOW=0 must be rejected")
	}
}

// TestMemGuardStartupGraceFailClosed: with no sample at all the guard starts
// unblocked, then fails closed once the grace period has elapsed, logging once.
func TestMemGuardStartupGraceFailClosed(t *testing.T) {
	g, buf := newTestGuard(640, 768, 384, 0)
	g.start = time.Now()
	if g.update(0, g.start.Add(1*time.Second)) {
		t.Fatal("inside the startup grace period must stay unblocked")
	}
	if strings.Contains(buf.String(), "fail-closed") {
		t.Fatal("must not fail closed inside the grace period")
	}
	if !g.update(0, g.start.Add(3*time.Second)) {
		t.Fatal("past the grace period with no sample must block")
	}
	if n := strings.Count(buf.String(), "fail-closed"); n != 1 {
		t.Fatalf("want exactly 1 fail-closed line, got %d: %q", n, buf.String())
	}
	if !g.update(0, g.start.Add(4*time.Second)) {
		t.Fatal("must stay blocked while no sample exists")
	}
	if n := strings.Count(buf.String(), "fail-closed"); n != 1 {
		t.Fatalf("fail-closed must log once, got %d: %q", n, buf.String())
	}
	// A healthy sample recovers.
	setSample(g, 1024*1024, g.start.Add(4*time.Second))
	if g.update(0, g.start.Add(4*time.Second)) {
		t.Fatal("a healthy sample must release the fail-closed block")
	}
	if n := strings.Count(buf.String(), "memguard fresh:"); n != 1 {
		t.Fatalf("want exactly 1 fresh line, got %d: %q", n, buf.String())
	}
}

func TestMemGuardNoSampleHardCapStillEngages(t *testing.T) {
	// A hard cap must engage even before any memory sample exists.
	capped, _ := newTestGuard(640, 768, 384, 10)
	capped.start = time.Now()
	if !capped.update(10, capped.start.Add(time.Millisecond)) {
		t.Fatal("the hard cap must still engage without a memory sample")
	}
}

// TestMemGuardCoherentSampleStaleness: the published sample carries its own
// monotonic timestamp, and staleness is derived from it rather than wall clock,
// so a healthy-looking but old value still fails safe.
func TestMemGuardCoherentSampleStaleness(t *testing.T) {
	g, buf := newTestGuard(640, 768, 384, 0)
	base := time.Now()
	setSample(g, 1024*1024, base)
	s := g.sample.Load()
	if s == nil || s.availKiB != 1024*1024 {
		t.Fatalf("sample not published: %+v", s)
	}
	if got := s.age(base.Add(500 * time.Millisecond)); got != 500*time.Millisecond {
		t.Fatalf("age: got %v want 500ms", got)
	}
	if got := s.age(base.Add(3 * time.Second)); got != 3*time.Second {
		t.Fatalf("age: got %v want 3s", got)
	}
	// The value and its timestamp are one immutable unit: 1 GiB is healthy,
	// but one millisecond past the stale bound it must block anyway.
	setSample(g, 1024*1024, base)
	if g.update(0, base.Add(memGuardStaleAfter)) {
		t.Fatal("exactly at the stale bound is not stale")
	}
	if !g.update(0, base.Add(memGuardStaleAfter+time.Millisecond)) {
		t.Fatal("past the stale bound must block (fail-safe)")
	}
	if n := strings.Count(buf.String(), "memguard stale:"); n != 1 {
		t.Fatalf("want exactly 1 stale line, got %d: %q", n, buf.String())
	}
	// A fresh sample recovers.
	setSample(g, 1024*1024, base.Add(memGuardStaleAfter+time.Millisecond))
	if g.update(0, base.Add(memGuardStaleAfter+time.Millisecond)) {
		t.Fatal("a fresh sample must release the fail-safe block")
	}
	if n := strings.Count(buf.String(), "memguard fresh:"); n != 1 {
		t.Fatalf("want exactly 1 fresh line, got %d: %q", n, buf.String())
	}
}

func TestMemGuardAcceptBlockedReevaluatesNewestSample(t *testing.T) {
	g, _ := newTestGuard(640, 768, 384, 0)
	now := time.Now()
	setSample(g, 1024*1024, now)
	if g.update(0, now) {
		t.Fatal("healthy must not block")
	}
	if g.acceptBlocked(0, now) {
		t.Fatal("healthy newest sample must not gate")
	}
	// A newer sample crosses low without update() running: the per-accept
	// re-evaluation must see it immediately.
	setSample(g, 639*1024, now)
	if !g.acceptBlocked(0, now) {
		t.Fatal("a newest sample below low must gate accepts")
	}
	setSample(g, 800*1024, now)
	if g.acceptBlocked(0, now) {
		t.Fatal("recovered newest sample must not gate")
	}
}

// TestMemGuardReAdmitTokenBucket: after a release the guard admits a 512-token
// burst, then refills at 512 per 100 ms for the 5 s window, and is unbounded
// afterwards.
func TestMemGuardReAdmitTokenBucket(t *testing.T) {
	g, _ := newTestGuard(16, 24, 8, 0)
	now := time.Now()
	setSample(g, 15*1024, now)
	if !g.update(0, now) {
		t.Fatal("must engage")
	}
	rel := now.Add(time.Millisecond)
	setSample(g, 24*1024, rel)
	if g.update(0, rel) {
		t.Fatal("must release")
	}
	if !rel.Before(g.reAdmitUntil) {
		t.Fatal("release must arm the re-admission window")
	}
	admitted := 0
	for g.admit(rel) {
		admitted++
		if admitted > memGuardReAdmitBurst+10 {
			t.Fatal("token bucket did not bound the burst")
		}
	}
	if admitted != memGuardReAdmitBurst {
		t.Fatalf("burst: got %d admits, want %d", admitted, memGuardReAdmitBurst)
	}
	if g.admit(rel) {
		t.Fatal("bucket must be empty after the burst")
	}
	if !g.reAdmitGated(rel) {
		t.Fatal("an empty bucket must gate the listener")
	}
	// One refill period yields one full burst.
	next := rel.Add(memGuardReAdmitPeriod)
	count := 0
	for g.admit(next) {
		count++
	}
	if count != memGuardReAdmitBurst {
		t.Fatalf("refill: got %d admits after one period, want %d", count, memGuardReAdmitBurst)
	}
	// Half a period refills about half the burst.
	half := next.Add(memGuardReAdmitPeriod / 2)
	count = 0
	for g.admit(half) {
		count++
	}
	if count < memGuardReAdmitBurst/2-1 || count > memGuardReAdmitBurst/2+1 {
		t.Fatalf("half-period refill: got %d admits, want ~%d", count, memGuardReAdmitBurst/2)
	}
	// After the window closes, admitting is unbounded again.
	after := rel.Add(memGuardReAdmitWindow + time.Millisecond)
	if g.reAdmitGated(after) {
		t.Fatal("after the window the listener must not be gated")
	}
	for i := 0; i < 4*memGuardReAdmitBurst; i++ {
		if !g.admit(after) {
			t.Fatalf("after the window admits must be unbounded (denied at %d)", i)
		}
	}
}

func TestMemGuardNilGuards(t *testing.T) {
	var g *memGuard
	if g.update(1<<30, time.Now()) || g.shedding() || g.shedSweepDue(time.Now()) || g.acceptBlocked(0, time.Now()) {
		t.Fatal("nil guard must not block, shed or gate")
	}
	if !g.admit(time.Now()) || g.reAdmitGated(time.Now()) {
		t.Fatal("nil guard must not limit re-admission")
	}
	g.noteShedClose(true) // must not panic
}

func TestStartStopMemGuard(t *testing.T) {
	f := openMemInfo()
	if f == nil {
		t.Skip("/proc/meminfo unavailable")
	}
	if _, ok := readMemAvailableKiB(f); !ok {
		f.Close()
		t.Skip("/proc/meminfo unreadable")
	}
	f.Close()
	buf := &bytes.Buffer{}
	g, e := startMemGuard(buf)
	if e != nil {
		t.Fatal(e)
	}
	if s := g.sample.Load(); s == nil || s.availKiB == 0 {
		t.Fatal("initial sample must be taken synchronously when the file opens")
	}
	g.stop()
	lines := strings.Split(strings.TrimSpace(buf.String()), "\n")
	if len(lines) != 1 || !strings.HasPrefix(lines[0], "memguard low=") {
		t.Fatalf("want exactly one startup config line, got %q", buf.String())
	}
	for _, field := range []string{"low=", "high=", "shed=", "max_conns=", "sample_interval=", "mem_available="} {
		if !strings.Contains(lines[0], field) {
			t.Fatalf("startup line missing %q: %q", field, lines[0])
		}
	}
	if g.sample.Load().availKiB >= g.hys.highKiB && g.update(0, time.Now()) {
		t.Fatal("a box above the high watermark must not start blocked")
	}
}
