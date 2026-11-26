package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"sync/atomic"
	"time"
)

// Memory guard defaults for the 1 vCPU / 2 GB challenge VM; the full derivation
// from results/remote-vm/ is in MEMGUARD.md.
//
//   - Idle MemAvailable on the 2 GB box is ~1,529-1,599 MiB.
//   - 80,000 keep-alive connections held MemAvailable >= ~1,173 MiB, so any
//     watermark must sit well below that and never engage at 80k.
//   - 160,000 connections drove MemAvailable to 148 MiB and the kernel
//     OOM-killed the server. Between t=60..90s TCP went 154,118 -> 160,456,
//     kernel TCP memory 294 -> 337 MiB, RSS 336 -> 429 MiB and MemAvailable
//     319 -> 157 MiB: the drain is dominated by in-flight work on existing
//     connections (~22 KiB per added connection during the ramp), not by the
//     accepts themselves.
//   - A competing build that held per-connection send buffers while blocked and
//     then re-admitted a burst after release was OOM-killed at ~158 s.
//
// Three watermarks drive three actions (all in MemAvailable KiB):
//
//	>= high            normal: accept freely
//	shed <= x < low    engaged: stop accepting, release idle buffers
//	< shed             emergency: also close connections
//
// low=640 MiB engages far above the 148 MiB kill point yet far below the
// 1,173 MiB floor of a passing 80k load. high=768 MiB (1.2x low) gives 128 MiB
// of hysteresis so memory returned in chunks cannot flap the guard around one
// threshold. shed=384 MiB is the last-ditch threshold: it sits above the
// ~254 MiB post-engage drain observed on the VM, so idle connections are closed
// while there is still headroom, and it releases back at low (256 MiB of shed
// hysteresis). The guard keys only off MemAvailable: MAX_CONNECTIONS is an
// opt-in hard cap (0, the default, means unlimited) and is never derived.
const (
	defaultMemGuardLowMiB  = 640
	defaultMemGuardHighMiB = 768
	defaultMemGuardShedMiB = 384
	// 100 ms keeps the worst-case reaction (sampler + loop wakeup) near 200 ms;
	// at the steepest observed 22-32 MiB/s drain that window costs <7 MiB. This
	// is nominal, not guaranteed: a burst inside one window can still overshoot.
	memGuardSampleInterval = 100 * time.Millisecond
	// If the last successful /proc/meminfo sample is older than this the guard
	// fails safe (blocks accepts) instead of trusting a stale reading forever.
	// The same grace period applies at startup: with no sample yet, accepts are
	// blocked once this much time has passed, until one arrives.
	memGuardStaleAfter = 2 * time.Second
	// A transition logs one line; this bounds repeated lines if an external
	// force (not the memory hysteresis) flaps the blocked state.
	memGuardLogInterval = time.Second
	// Emergency shedding bounds: at most memGuardShedBatch idle connections are
	// closed per sweep, and sweeps happen at most once per memGuardShedInterval.
	memGuardShedBatch    = 2048
	memGuardShedInterval = 100 * time.Millisecond
	// Gradual re-admission: after the guard releases, new connections are
	// admitted through a token bucket (burst memGuardReAdmitBurst, refilled at
	// that many per memGuardReAdmitPeriod) for memGuardReAdmitWindow. This
	// prevents the release-then-burst pattern that OOM-killed a competing build.
	memGuardReAdmitWindow = 5 * time.Second
	memGuardReAdmitBurst  = 512
	memGuardReAdmitPeriod = 100 * time.Millisecond
	memInfoPath           = "/proc/meminfo"
	// Values above this are rejected as overflow / configuration mistakes.
	memGuardEnvMaxMiB uint64 = 1 << 20
)

// processStart is the monotonic origin for sample ages: time.Since(processStart)
// reads the monotonic clock and is safe to compare across goroutines.
var processStart = time.Now()

func monoNanos() int64 { return time.Since(processStart).Nanoseconds() }

// memSample is one immutable MemAvailable reading published through an
// atomic.Pointer: the value and its monotonic timestamp become visible
// together, so the loop never observes a value paired with the wrong time.
type memSample struct {
	availKiB  uint64
	monoNanos int64
}

// age reports how long ago the sample was taken relative to now. Both times are
// derived from the same monotonic origin, so no wall-clock source is involved.
func (s *memSample) age(now time.Time) time.Duration {
	if s == nil {
		return 0
	}
	return time.Duration(now.Sub(processStart).Nanoseconds() - s.monoNanos)
}

// hysteresis is a two-threshold state machine over MemAvailable in KiB.
// It engages strictly below low and releases at or above high > low.
// low == 0 disables this half of the guard entirely.
type hysteresis struct {
	lowKiB, highKiB uint64
	engaged         bool
}

func (h *hysteresis) update(availKiB uint64) bool {
	if h.lowKiB == 0 {
		return false
	}
	if h.engaged {
		if availKiB >= h.highKiB {
			h.engaged = false
		}
	} else if availKiB < h.lowKiB {
		h.engaged = true
	}
	return h.engaged
}

// parseFieldKiB extracts a "<name>: <n> kB" field (in KiB) from /proc/meminfo
// content. It reports false when the field is absent or malformed.
func parseFieldKiB(data []byte, prefix string) (uint64, bool) {
	for _, line := range strings.Split(string(data), "\n") {
		if !strings.HasPrefix(line, prefix) {
			continue
		}
		fields := strings.Fields(strings.TrimPrefix(line, prefix))
		if len(fields) == 0 {
			return 0, false
		}
		v, e := strconv.ParseUint(fields[0], 10, 64)
		return v, e == nil
	}
	return 0, false
}

func parseMemAvailableKiB(data []byte) (uint64, bool) {
	return parseFieldKiB(data, "MemAvailable:")
}

// readMemInfo preads the already-open descriptor at offset 0. procfs regenerates
// the file on every read and ignores the offset, so one retained descriptor
// needs no reopen and costs no open() (which can fail under EMFILE).
func readMemInfo(f *os.File) ([]byte, bool) {
	if f == nil {
		return nil, false
	}
	buf := make([]byte, 8192)
	n, _ := f.ReadAt(buf, 0)
	if n <= 0 {
		return nil, false
	}
	return buf[:n], true
}

func readMemAvailableKiB(f *os.File) (uint64, bool) {
	data, ok := readMemInfo(f)
	if !ok {
		return 0, false
	}
	return parseMemAvailableKiB(data)
}

func openMemInfo() *os.File {
	f, e := os.Open(memInfoPath)
	if e != nil {
		return nil
	}
	return f
}

// memGuard tracks system memory headroom and open connection count so the
// network loops can stop accepting new clients before the kernel OOM killer
// fires. Refusing connections is allowed by the challenge rules; being
// OOM-killed loses the run.
//
// Concurrency: a sampler goroutine preads /proc/meminfo through one retained fd
// at most every 100 ms and publishes an immutable memSample pointer; everything
// else (hysteresis, blocked/shed state, logging, staleness, the re-admission
// token bucket) is owned and mutated exclusively by the single network loop
// thread through update and the accept-path helpers, so no locks are needed.
type memGuard struct {
	lowMiB, highMiB, shedMiB uint64
	maxConns                 int64 // 0 means unlimited
	out                      io.Writer
	// sample is the only cross-thread state: written by the sampler, read by
	// the loop thread.
	sample      atomic.Pointer[memSample]
	samplerStop chan struct{}
	samplerDone chan struct{}
	// meminfo is opened once at startup and pread per sample; if it could not
	// be opened the sampler retries. Owned by the sampler after start.
	meminfo *os.File
	start   time.Time
	// Loop-thread-only state below.
	hys            hysteresis
	shed           hysteresis
	blocked        bool
	shedOn         bool
	stale          bool
	noSample       bool
	lastEngageLog  time.Time
	lastReleaseLog time.Time
	lastShedSweep  time.Time
	// Gradual re-admission token bucket (loop thread only).
	reAdmitUntil  time.Time
	reAdmitTokens float64
	reAdmitAt     time.Time
	// Shed episode accounting (loop thread only).
	shedClosedIdle   uint64
	shedClosedActive uint64
}

// memGuardConfig validates the environment. MAX_CONNECTIONS is the opt-in hard
// connection cap: unset or "0" means unlimited. MEMGUARD_SHED_MIB must be 0 or
// strictly below MEMGUARD_LOW_MIB.
func memGuardConfig() (lowMiB, highMiB, shedMiB uint64, maxConns int64, e error) {
	lowMiB, highMiB, shedMiB = defaultMemGuardLowMiB, defaultMemGuardHighMiB, defaultMemGuardShedMiB
	if v := os.Getenv("MEMGUARD_LOW_MIB"); v != "" {
		if lowMiB, e = strconv.ParseUint(v, 10, 64); e != nil {
			return 0, 0, 0, 0, errors.New("MEMGUARD_LOW_MIB must be a non-negative integer")
		}
		if lowMiB > memGuardEnvMaxMiB {
			return 0, 0, 0, 0, fmt.Errorf("MEMGUARD_LOW_MIB must be <= %d", memGuardEnvMaxMiB)
		}
	}
	if v := os.Getenv("MEMGUARD_HIGH_MIB"); v != "" {
		if highMiB, e = strconv.ParseUint(v, 10, 64); e != nil {
			return 0, 0, 0, 0, errors.New("MEMGUARD_HIGH_MIB must be a non-negative integer")
		}
		if highMiB > memGuardEnvMaxMiB {
			return 0, 0, 0, 0, fmt.Errorf("MEMGUARD_HIGH_MIB must be <= %d", memGuardEnvMaxMiB)
		}
	}
	shedExplicit := false
	if v := os.Getenv("MEMGUARD_SHED_MIB"); v != "" {
		shedExplicit = true
		if shedMiB, e = strconv.ParseUint(v, 10, 64); e != nil {
			return 0, 0, 0, 0, errors.New("MEMGUARD_SHED_MIB must be a non-negative integer")
		}
		if shedMiB > memGuardEnvMaxMiB {
			return 0, 0, 0, 0, fmt.Errorf("MEMGUARD_SHED_MIB must be <= %d", memGuardEnvMaxMiB)
		}
	}
	if lowMiB > 0 && highMiB <= lowMiB {
		return 0, 0, 0, 0, errors.New("MEMGUARD_HIGH_MIB must be greater than MEMGUARD_LOW_MIB, or MEMGUARD_LOW_MIB=0 disables the memory watermark")
	}
	if lowMiB == 0 {
		// low=0 disables the memory watermark, and shedding lives beneath it.
		if shedExplicit && shedMiB != 0 {
			return 0, 0, 0, 0, errors.New("MEMGUARD_SHED_MIB must be 0 when MEMGUARD_LOW_MIB=0 disables the memory watermark")
		}
		shedMiB = 0
	} else if shedMiB != 0 && shedMiB >= lowMiB {
		return 0, 0, 0, 0, errors.New("MEMGUARD_SHED_MIB must be less than MEMGUARD_LOW_MIB (0 disables emergency shedding)")
	}
	if v, ok := os.LookupEnv("MAX_CONNECTIONS"); ok && v != "" {
		if maxConns, e = strconv.ParseInt(v, 10, 64); e != nil || maxConns < 0 {
			return 0, 0, 0, 0, errors.New("MAX_CONNECTIONS must be a non-negative integer (0 means unlimited)")
		}
	}
	return lowMiB, highMiB, shedMiB, maxConns, nil
}

// publish makes one immutable sample visible to the loop thread.
func (g *memGuard) publish(availKiB uint64) {
	g.sample.Store(&memSample{availKiB: availKiB, monoNanos: monoNanos()})
}

// startMemGuard validates the environment, opens /proc/meminfo once, takes the
// first sample if it can (a failure here does NOT prevent startup: the guard
// starts unblocked, fails closed after memGuardStaleAfter if no sample ever
// arrives, and the sampler keeps retrying), starts the sampler and logs the
// chosen configuration. The guard keys only off MemAvailable; MAX_CONNECTIONS
// is an opt-in hard cap and 0 (the default) means unlimited.
func startMemGuard(out io.Writer) (*memGuard, error) {
	low, high, shed, maxConns, e := memGuardConfig()
	if e != nil {
		return nil, e
	}
	g := &memGuard{
		lowMiB: low, highMiB: high, shedMiB: shed, maxConns: maxConns, out: out,
		hys:         hysteresis{lowKiB: low * 1024, highKiB: high * 1024},
		shed:        hysteresis{lowKiB: shed * 1024, highKiB: low * 1024},
		samplerStop: make(chan struct{}),
		samplerDone: make(chan struct{}),
		start:       time.Now(),
	}
	g.meminfo = openMemInfo()
	var firstKiB uint64
	var gotFirst bool
	if g.meminfo != nil {
		firstKiB, gotFirst = readMemAvailableKiB(g.meminfo)
	}
	if gotFirst {
		g.publish(firstKiB)
	}
	go g.sampleLoop()
	avail := "unknown"
	if gotFirst {
		avail = fmt.Sprintf("%dMiB", firstKiB/1024)
	}
	fmt.Fprintf(out, "memguard low=%dMiB high=%dMiB shed=%dMiB max_conns=%d sample_interval=%s mem_available=%s\n",
		low, high, shed, maxConns, memGuardSampleInterval, avail)
	return g, nil
}

func (g *memGuard) sampleLoop() {
	defer close(g.samplerDone)
	t := time.NewTicker(memGuardSampleInterval)
	defer t.Stop()
	for {
		select {
		case <-g.samplerStop:
			return
		case <-t.C:
			// Keep retrying the open so a startup EMFILE does not disable the
			// guard permanently. A transient read failure keeps the last value
			// until it ages past memGuardStaleAfter, when update() fails safe.
			if g.meminfo == nil {
				if g.meminfo = openMemInfo(); g.meminfo == nil {
					continue
				}
			}
			if v, ok := readMemAvailableKiB(g.meminfo); ok {
				g.publish(v)
			}
		}
	}
}

func (g *memGuard) stop() {
	if g == nil {
		return
	}
	close(g.samplerStop)
	<-g.samplerDone
	if g.meminfo != nil {
		_ = g.meminfo.Close()
		g.meminfo = nil
	}
}

// capReached reports whether the hard connection cap is hit, so accepting
// must stop. overCap reports whether open already exceeds the cap, used to
// discard the small in-flight overshoot of a batch or multishot accept that
// was issued before the cap was observed. Both are nil-safe so backends
// without a guard keep their original behaviour.
func (g *memGuard) capReached(open int) bool {
	return g != nil && g.maxConns > 0 && int64(open) >= g.maxConns
}
func (g *memGuard) overCap(open int) bool {
	return g != nil && g.maxConns > 0 && int64(open) > g.maxConns
}

// update is called by the network loop thread at least once per iteration; it
// performs no syscall (only atomic loads). open is the loop's current count of
// open connections. It reports whether accepting new connections must be
// blocked.
//
// Fail-safe: if a sample has been read at least once but the last successful
// sample is older than memGuardStaleAfter, accepts are blocked even though the
// last value may look healthy — a stale reading must never be trusted forever.
// Before the very first sample the guard stays unblocked for the startup grace
// period, then fails closed until a sample arrives.
func (g *memGuard) update(open int, now time.Time) bool {
	if g == nil {
		return false
	}
	blocked := g.capReached(open)
	s := g.sample.Load()
	stale, noSample := false, false
	if s == nil {
		if now.Sub(g.start) >= memGuardStaleAfter {
			noSample = true
			blocked = true
		}
	} else {
		stale = s.age(now) > memGuardStaleAfter
		blocked = g.hys.update(s.availKiB) || stale || blocked
		if !stale {
			// Shedding is destructive, so only a fresh sample may change it.
			blocked = g.shed.update(s.availKiB) || blocked
		}
	}
	availMiB := uint64(0)
	if s != nil {
		availMiB = s.availKiB / 1024
	}
	if noSample != g.noSample {
		g.noSample = noSample
		if noSample {
			fmt.Fprintf(g.out, "memguard fail-closed: no MemAvailable sample within %s of startup, blocking accepts conns=%d\n",
				memGuardStaleAfter, open)
		} else {
			fmt.Fprintf(g.out, "memguard fresh: first MemAvailable sample arrived mem_available=%dMiB conns=%d\n",
				availMiB, open)
		}
	}
	if stale != g.stale {
		g.stale = stale
		if stale {
			fmt.Fprintf(g.out, "memguard stale: no MemAvailable sample for >%s, blocking accepts (fail-safe) conns=%d max_conns=%d\n",
				memGuardStaleAfter, open, g.maxConns)
		} else {
			fmt.Fprintf(g.out, "memguard fresh: MemAvailable sampling recovered mem_available=%dMiB conns=%d\n",
				availMiB, open)
		}
	}
	if g.shed.engaged != g.shedOn {
		g.shedOn = g.shed.engaged
		if g.shedOn {
			fmt.Fprintf(g.out, "memguard shed engaged: mem_available=%dMiB shed=%dMiB low=%dMiB conns=%d\n",
				availMiB, g.shedMiB, g.lowMiB, open)
		} else {
			fmt.Fprintf(g.out, "memguard shed released: mem_available=%dMiB shed=%dMiB low=%dMiB conns=%d closed_idle=%d closed_after_response=%d\n",
				availMiB, g.shedMiB, g.lowMiB, open, g.shedClosedIdle, g.shedClosedActive)
		}
		g.shedClosedIdle, g.shedClosedActive = 0, 0
	}
	if blocked == g.blocked {
		return blocked
	}
	g.blocked = blocked
	if blocked {
		if now.Sub(g.lastEngageLog) >= memGuardLogInterval {
			g.lastEngageLog = now
			fmt.Fprintf(g.out, "memguard engaged: mem_available=%dMiB low=%dMiB high=%dMiB shed=%dMiB conns=%d max_conns=%d stale=%t\n",
				availMiB, g.lowMiB, g.highMiB, g.shedMiB, open, g.maxConns, stale)
		}
	} else {
		// Gradual re-admission: every release starts a fresh window so a burst
		// of new connections cannot undo the release while in-flight work on
		// established connections is still draining.
		g.reAdmitUntil = now.Add(memGuardReAdmitWindow)
		g.reAdmitTokens = memGuardReAdmitBurst
		g.reAdmitAt = now
		if now.Sub(g.lastReleaseLog) >= memGuardLogInterval {
			g.lastReleaseLog = now
			fmt.Fprintf(g.out, "memguard released: mem_available=%dMiB low=%dMiB high=%dMiB conns=%d max_conns=%d readmit_window=%s\n",
				availMiB, g.lowMiB, g.highMiB, open, g.maxConns, memGuardReAdmitWindow)
		}
	}
	return blocked
}

// acceptBlocked is the cheap per-accept re-evaluation used inside an accept
// batch: one atomic pointer load plus a few comparisons. It mutates no state
// and logs nothing; the full transition is performed by update at the loop
// iteration boundary. It reports true while the guard is engaged (memory, hard
// cap or stale sample) or once the newest published sample is itself below the
// low watermark.
func (g *memGuard) acceptBlocked(open int, now time.Time) bool {
	if g == nil {
		return false
	}
	if g.blocked || g.capReached(open) {
		return true
	}
	s := g.sample.Load()
	if s == nil {
		return false
	}
	if s.age(now) > memGuardStaleAfter {
		return true
	}
	return s.availKiB < g.hys.lowKiB
}

// shedding reports whether the emergency shedder is engaged.
func (g *memGuard) shedding() bool { return g != nil && g.shed.engaged }

// shedSweepDue rate-limits idle-connection sweeps to one per
// memGuardShedInterval; the first call after entering shed mode is always due.
func (g *memGuard) shedSweepDue(now time.Time) bool {
	if g == nil || !g.shed.engaged {
		return false
	}
	if now.Sub(g.lastShedSweep) < memGuardShedInterval {
		return false
	}
	g.lastShedSweep = now
	return true
}

// noteShedClose records one connection closed by the shedder for the episode
// summary logged on release. idle is true for an immediately-closed idle
// connection and false for one closed right after its in-flight response.
func (g *memGuard) noteShedClose(idle bool) {
	if g == nil {
		return
	}
	if idle {
		g.shedClosedIdle++
	} else {
		g.shedClosedActive++
	}
}

// refill advances the re-admission token bucket to now.
func (g *memGuard) refill(now time.Time) {
	if elapsed := now.Sub(g.reAdmitAt); elapsed > 0 {
		g.reAdmitTokens += float64(elapsed) * (float64(memGuardReAdmitBurst) / float64(memGuardReAdmitPeriod))
		if g.reAdmitTokens > memGuardReAdmitBurst {
			g.reAdmitTokens = memGuardReAdmitBurst
		}
		g.reAdmitAt = now
	}
}

// admit consumes one re-admission token. It reports true (unbounded) outside
// the re-admission window, so normal operation is unaffected.
func (g *memGuard) admit(now time.Time) bool {
	if g == nil || !now.Before(g.reAdmitUntil) {
		return true
	}
	g.refill(now)
	if g.reAdmitTokens >= 1 {
		g.reAdmitTokens--
		return true
	}
	return false
}

// reAdmitGated reports whether accepting should be paused solely because the
// gradual re-admission budget is momentarily exhausted. The loops use it to
// mask the listener so the accept path cannot spin while tokens refill.
func (g *memGuard) reAdmitGated(now time.Time) bool {
	if g == nil || !now.Before(g.reAdmitUntil) {
		return false
	}
	g.refill(now)
	return g.reAdmitTokens < 1
}
