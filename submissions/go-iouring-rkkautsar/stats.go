package main

import (
	"log"
	"os"
	"strconv"
	"strings"
	"sync/atomic"
	"time"
)

var (
	statEnabled     atomic.Bool
	statEnter       atomic.Uint64
	statEnterNS     atomic.Uint64
	statCQEs        atomic.Uint64
	statWriteJob    atomic.Uint64
	statWriteReq    atomic.Uint64
	statWriteNS     atomic.Uint64
	statCreateJob   atomic.Uint64
	statCreateNS    atomic.Uint64
	statLikeJob     atomic.Uint64
	statLikeNS      atomic.Uint64
	statLikeExistNS atomic.Uint64
	statLikeInsNS   atomic.Uint64
	statCommitNS    atomic.Uint64
)

func startBtreeStats() {
	if os.Getenv("STATS") != "1" {
		return
	}
	statEnabled.Store(true)
	enableBtreeStats()
	go func() {
		t := time.NewTicker(10 * time.Second)
		defer t.Stop()
		for range t.C {
			u, s := procCPU()
			b := readBtreeStats()
			log.Printf("stat enter_calls=%d enter_ms=%d cqes=%d feed_calls=%d feed_ms=%d post_calls=%d post_ms=%d write_jobs=%d write_reqs=%d write_ms=%d create_jobs=%d create_ms=%d like_jobs=%d like_ms=%d like_exist_ms=%d like_insert_ms=%d commit_ms=%d user_ticks=%d sys_ticks=%d",
				statEnter.Load(), statEnterNS.Load()/1e6, statCQEs.Load(),
				b[0], b[1]/1e6, b[2], b[3]/1e6,
				statWriteJob.Load(), statWriteReq.Load(), statWriteNS.Load()/1e6,
				statCreateJob.Load(), statCreateNS.Load()/1e6,
				statLikeJob.Load(), statLikeNS.Load()/1e6,
				statLikeExistNS.Load()/1e6, statLikeInsNS.Load()/1e6,
				statCommitNS.Load()/1e6, u, s,
			)
		}
	}()
}

func procCPU() (uint64, uint64) {
	b, err := os.ReadFile("/proc/self/stat")
	if err != nil {
		return 0, 0
	}
	s := string(b)
	i := strings.LastIndexByte(s, ')')
	if i < 0 || i+2 >= len(s) {
		return 0, 0
	}
	f := strings.Fields(s[i+2:])
	if len(f) < 13 {
		return 0, 0
	}
	u, _ := strconv.ParseUint(f[11], 10, 64)
	sy, _ := strconv.ParseUint(f[12], 10, 64)
	return u, sy
}
