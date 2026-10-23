package main

import (
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"runtime/pprof"
	"sync"
	"time"
)

type runProfiles struct {
	cpu               *os.File
	cpuPath, heapPath string
	output            io.Writer
	started           time.Time
	initial           runtime.MemStats
	once              sync.Once
	err               error
}

func startProfiles(cpu, heap string, output io.Writer) (*runProfiles, error) {
	if cpu == "" && heap == "" {
		return nil, nil
	}
	if cpu != "" && heap != "" {
		a, err := filepath.Abs(cpu)
		if err != nil {
			return nil, err
		}
		b, err := filepath.Abs(heap)
		if err != nil {
			return nil, err
		}
		if a == b {
			return nil, errors.New("CPU_PROFILE and HEAP_PROFILE must use different files")
		}
	}
	p := &runProfiles{cpuPath: cpu, heapPath: heap, output: output, started: time.Now()}
	runtime.ReadMemStats(&p.initial)
	if cpu != "" {
		file, err := profileFile(cpu)
		if err != nil {
			return nil, err
		}
		if err = pprof.StartCPUProfile(file); err != nil {
			file.Close()
			return nil, err
		}
		p.cpu = file
	}
	return p, nil
}
func profileFile(path string) (*os.File, error) {
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return nil, err
	}
	return os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0600)
}
func (p *runProfiles) Close() error {
	if p == nil {
		return nil
	}
	p.once.Do(func() {
		if p.cpu != nil {
			pprof.StopCPUProfile()
			p.err = errors.Join(p.err, p.cpu.Close())
		}
		if p.heapPath != "" {
			file, err := profileFile(p.heapPath)
			if err == nil {
				err = errors.Join(pprof.WriteHeapProfile(file), file.Close())
			}
			p.err = errors.Join(p.err, err)
		}
		var m runtime.MemStats
		runtime.ReadMemStats(&m)
		var recentMax uint64
		for _, pause := range m.PauseNs {
			if pause > recentMax {
				recentMax = pause
			}
		}
		report := map[string]any{
			"kind": "runtime_profile_summary", "cpu_profile": p.cpuPath, "heap_profile": p.heapPath,
			"elapsed_s": time.Since(p.started).Seconds(), "goroutines": runtime.NumGoroutine(),
			"gomaxprocs": runtime.GOMAXPROCS(0), "num_gc": m.NumGC, "num_gc_during_profile": m.NumGC - p.initial.NumGC,
			"num_forced_gc": m.NumForcedGC, "gc_cpu_fraction": m.GCCPUFraction,
			"gc_pause_total_ns": m.PauseTotalNs, "gc_pause_during_profile_ns": m.PauseTotalNs - p.initial.PauseTotalNs,
			"recent_256_gc_pause_max_ns": recentMax, "heap_alloc_bytes": m.HeapAlloc, "heap_inuse_bytes": m.HeapInuse,
			"heap_sys_bytes": m.HeapSys, "runtime_sys_bytes": m.Sys, "total_alloc_bytes": m.TotalAlloc,
			"allocated_during_profile_bytes": m.TotalAlloc - p.initial.TotalAlloc, "mallocs": m.Mallocs, "frees": m.Frees,
		}
		if p.err != nil {
			report["profile_error"] = p.err.Error()
		}
		p.err = errors.Join(p.err, json.NewEncoder(p.output).Encode(report))
	})
	return p.err
}
