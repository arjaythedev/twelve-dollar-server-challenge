package main

import (
	"bytes"
	"compress/gzip"
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
	"runtime/pprof"
	"testing"
)

func assertProfileFlushed(t *testing.T, path string) {
	t.Helper()
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	gz, err := gzip.NewReader(file)
	if err != nil {
		t.Fatalf("incomplete profile %s: %v", path, err)
	}
	defer gz.Close()
	if data, err := io.ReadAll(gz); err != nil || len(data) == 0 {
		t.Fatalf("incomplete profile %s: len=%d err=%v", path, len(data), err)
	}
}

// Catches accidental default profiling, output, or a nil receiver panic.
func TestProfilesDisabled(t *testing.T) {
	var output bytes.Buffer
	p, err := startProfiles("", "", &output)
	if err != nil || p != nil {
		t.Fatalf("default profiling active: p=%v err=%v", p, err)
	}
	if err := p.Close(); err != nil || output.Len() != 0 {
		t.Fatalf("disabled close: err=%v output=%q", err, output.String())
	}
}

// Catches missing StopCPUProfile, missing heap output, and duplicate Close writes.
func TestProfilesFlushOnce(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "nested")
	cpu, heap := filepath.Join(dir, "cpu.pprof"), filepath.Join(dir, "heap.pprof")
	var output bytes.Buffer
	p, err := startProfiles(cpu, heap, &output)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { p.Close() })
	if err := p.Close(); err != nil {
		t.Fatal(err)
	}
	first := output.String()
	if err := p.Close(); err != nil || output.String() != first {
		t.Fatalf("close is not idempotent: err=%v output=%s", err, output.String())
	}
	for _, path := range []string{cpu, heap} {
		assertProfileFlushed(t, path)
	}
	var report map[string]any
	if err := json.Unmarshal(output.Bytes(), &report); err != nil {
		t.Fatal(err)
	}
	if report["kind"] != "runtime_profile_summary" || report["cpu_profile"] != cpu || report["heap_profile"] != heap {
		t.Fatalf("missing profile summary: %v", report)
	}
}

// Catches heap output silently overwriting the CPU profile, including lexical aliases.
func TestProfilePathsMustDiffer(t *testing.T) {
	dir := t.TempDir()
	cpu := filepath.Join(dir, "profile.pprof")
	for _, heap := range []string{cpu, dir + "/unused/../profile.pprof"} {
		p, err := startProfiles(cpu, heap, io.Discard)
		if err == nil {
			p.Close()
			t.Fatalf("accepted overlapping profile paths: %q, %q", cpu, heap)
		}
	}
	if _, err := os.Stat(cpu); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("invalid configuration created output: %v", err)
	}
}

// Catches discarded startup errors from the filesystem and the global CPU profiler.
func TestProfilesStartupErrors(t *testing.T) {
	blocker := filepath.Join(t.TempDir(), "file")
	if err := os.WriteFile(blocker, nil, 0600); err != nil {
		t.Fatal(err)
	}
	if p, err := startProfiles(filepath.Join(blocker, "cpu.pprof"), "", io.Discard); err == nil || p != nil {
		if p != nil {
			p.Close()
		}
		t.Fatalf("accepted unwritable CPU path: p=%v err=%v", p, err)
	}
	if err := pprof.StartCPUProfile(io.Discard); err != nil {
		t.Fatal(err)
	}
	defer pprof.StopCPUProfile()
	p, err := startProfiles(filepath.Join(t.TempDir(), "cpu.pprof"), "", io.Discard)
	if err == nil || p != nil {
		if p != nil {
			p.Close()
		}
		t.Fatalf("accepted an already active CPU profiler: p=%v err=%v", p, err)
	}
}

// Catches a failed heap write hiding the error or preventing CPU flush.
func TestProfilesClosePreservesErrorsAndFlushesCPU(t *testing.T) {
	dir := t.TempDir()
	cpu, blocker := filepath.Join(dir, "cpu.pprof"), filepath.Join(dir, "file")
	if err := os.WriteFile(blocker, nil, 0600); err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	p, err := startProfiles(cpu, filepath.Join(blocker, "heap.pprof"), &output)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { p.Close() })
	first := p.Close()
	if first == nil || p.Close() != first {
		t.Fatalf("lost close error: first=%v second=%v", first, p.Close())
	}
	assertProfileFlushed(t, cpu)
	var report map[string]any
	if err := json.Unmarshal(output.Bytes(), &report); err != nil || report["profile_error"] == nil {
		t.Fatalf("missing profile error: report=%v err=%v", report, err)
	}
}

func TestProfilesReportWriteError(t *testing.T) {
	output, err := os.CreateTemp(t.TempDir(), "closed-output")
	if err != nil {
		t.Fatal(err)
	}
	output.Close()
	heap := filepath.Join(t.TempDir(), "heap.pprof")
	p, err := startProfiles("", heap, output)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { p.Close() })
	if err := p.Close(); !errors.Is(err, os.ErrClosed) {
		t.Fatalf("lost summary write error: %v", err)
	}
	assertProfileFlushed(t, heap)
}
