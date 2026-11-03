//go:build ignore

package main

import (
	"bufio"
	"bytes"
	"crypto/tls"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

type TokenUser struct {
	ID       int    `json:"id"`
	Username string `json:"username"`
	Token    string `json:"token"`
}

// Latency bucket boundaries in microseconds
var bucketLimits = []int64{
	500,      // 0.5ms
	1000,     // 1ms
	2000,     // 2ms
	5000,     // 5ms
	10000,    // 10ms
	20000,    // 20ms
	50000,    // 50ms
	100000,   // 100ms
	150000,   // 150ms
	200000,   // 200ms
	250000,   // 250ms
	300000,   // 300ms
	350000,   // 350ms
	400000,   // 400ms
	450000,   // 450ms
	500000,   // 500ms (Threshold p95)
	600000,   // 600ms
	750000,   // 750ms
	1000000,  // 1000ms (Threshold p99)
	1500000,  // 1.5s
	2000000,  // 2s
	3000000,  // 3s
	5000000,  // 5s
	10000000, // 10s
}

const numBuckets = 25 // 24 limits + 1 overflow bucket

type Histogram struct {
	counts [numBuckets]atomic.Uint64
	total  atomic.Uint64
	sumUs  atomic.Uint64
	maxUs  atomic.Int64
}

func (h *Histogram) Record(us int64) {
	h.total.Add(1)
	h.sumUs.Add(uint64(us))

	// Update max atomically
	for {
		cur := h.maxUs.Load()
		if us <= cur {
			break
		}
		if h.maxUs.CompareAndSwap(cur, us) {
			break
		}
	}

	for i, lim := range bucketLimits {
		if us <= lim {
			h.counts[i].Add(1)
			return
		}
	}
	h.counts[numBuckets-1].Add(1)
}

func (h *Histogram) Percentile(p float64) time.Duration {
	tot := h.total.Load()
	if tot == 0 {
		return 0
	}
	target := uint64(float64(tot) * p)
	if target == 0 {
		target = 1
	}
	var acc uint64
	for i := 0; i < len(bucketLimits); i++ {
		acc += h.counts[i].Load()
		if acc >= target {
			return time.Duration(bucketLimits[i]) * time.Microsecond
		}
	}
	return time.Duration(h.maxUs.Load()) * time.Microsecond
}

func (h *Histogram) Avg() time.Duration {
	tot := h.total.Load()
	if tot == 0 {
		return 0
	}
	return time.Duration(h.sumUs.Load()/tot) * time.Microsecond
}

func (h *Histogram) Max() time.Duration {
	return time.Duration(h.maxUs.Load()) * time.Microsecond
}

// ============================================================================
// ZERO-ALLOCATION PERSISTENT-FD /proc TELEMETRY & CPU AFFINITY ENGINE
// ============================================================================

// ProcFDs holds persistent open file descriptors to /proc/<pid>/* files.
// Using syscall.Pread at offset 0 avoids open/close/fstat syscalls and heap allocations.
type ProcFDs struct {
	pid      int
	statFD   int
	statusFD int
	ioFD     int
}

func openProcFDs(pid int) *ProcFDs {
	if pid <= 0 {
		return nil
	}
	p := &ProcFDs{
		pid:      pid,
		statFD:   -1,
		statusFD: -1,
		ioFD:     -1,
	}
	if fd, err := syscall.Open(fmt.Sprintf("/proc/%d/stat", pid), syscall.O_RDONLY, 0); err == nil {
		p.statFD = fd
	}
	if fd, err := syscall.Open(fmt.Sprintf("/proc/%d/status", pid), syscall.O_RDONLY, 0); err == nil {
		p.statusFD = fd
	}
	if fd, err := syscall.Open(fmt.Sprintf("/proc/%d/io", pid), syscall.O_RDONLY, 0); err == nil {
		p.ioFD = fd
	}
	return p
}

func (p *ProcFDs) Close() {
	if p == nil {
		return
	}
	if p.statFD >= 0 {
		_ = syscall.Close(p.statFD)
	}
	if p.statusFD >= 0 {
		_ = syscall.Close(p.statusFD)
	}
	if p.ioFD >= 0 {
		_ = syscall.Close(p.ioFD)
	}
}

type ProcSnapshot struct {
	utimeTicks    uint64 // User CPU ticks (1/100s)
	stimeTicks    uint64 // Kernel/System CPU ticks (1/100s)
	numThreads    int64  // Active OS threads
	lastCPU       int64  // Last logical CPU core executed on
	vmRSSKb       int64  // Current Physical RAM (kB)
	vmHWMKb       int64  // Peak Physical RAM (High Water Mark, kB)
	rssAnonKb     int64  // Anonymous Heap + Stack RAM (kB)
	rssFileKb     int64  // Memory-mapped file cache RAM (e.g. SQLite mmap, kB)
	volCtxtSw     uint64 // Voluntary context switches
	nonvolCtxtSw  uint64 // Involuntary context switches (CPU preemptions)
	rcharBytes    uint64 // Logical bytes read via syscalls
	wcharBytes    uint64 // Logical bytes written via syscalls
	syscr         uint64 // Read syscalls
	syscw         uint64 // Write syscalls
	readBytes     uint64 // Physical block storage bytes read
	writeBytes    uint64 // Physical block storage bytes written (SQLite WAL/Checkpoints)
	cpusAllowed   string // Allowed CPU list (sampled once at startup/end)
}

// parseUintZeroAlloc parses the first unsigned integer found in b without heap allocation.
func parseUintZeroAlloc(b []byte) uint64 {
	var v uint64
	started := false
	for i := 0; i < len(b); i++ {
		c := b[i]
		if c >= '0' && c <= '9' {
			started = true
			v = v*10 + uint64(c-'0')
		} else if started {
			break
		}
	}
	return v
}

// sampleProcFast reads /proc/<pid>/{stat,status,io} via syscall.Pread into buf with 0 heap allocations.
func sampleProcFast(p *ProcFDs, buf []byte, snap *ProcSnapshot) bool {
	if p == nil || p.statFD < 0 {
		return false
	}

	// 1. Parse /proc/<pid>/stat
	n, err := syscall.Pread(p.statFD, buf, 0)
	if err != nil || n <= 0 {
		return false
	}
	data := buf[:n]
	closeParen := bytes.LastIndexByte(data, ')')
	if closeParen > 0 && closeParen+2 < len(data) {
		rest := data[closeParen+2:]
		// Field 3 (state) is index 0 in rest.
		// utime = field 14 (idx 11), stime = field 15 (idx 12), num_threads = field 20 (idx 17), processor = field 39 (idx 36)
		fieldIdx := 0
		i := 0
		for i < len(rest) && fieldIdx <= 36 {
			for i < len(rest) && rest[i] == ' ' {
				i++
			}
			start := i
			for i < len(rest) && rest[i] != ' ' && rest[i] != '\n' {
				i++
			}
			if start < i {
				switch fieldIdx {
				case 11:
					snap.utimeTicks = parseUintZeroAlloc(rest[start:i])
				case 12:
					snap.stimeTicks = parseUintZeroAlloc(rest[start:i])
				case 17:
					snap.numThreads = int64(parseUintZeroAlloc(rest[start:i]))
				case 36:
					snap.lastCPU = int64(parseUintZeroAlloc(rest[start:i]))
				}
				fieldIdx++
			}
		}
	}

	// 2. Parse /proc/<pid>/status
	if p.statusFD >= 0 {
		n, err = syscall.Pread(p.statusFD, buf, 0)
		if err == nil && n > 0 {
			sdata := buf[:n]
			lineStart := 0
			for i := 0; i < len(sdata); i++ {
				if sdata[i] == '\n' {
					line := sdata[lineStart:i]
					if len(line) > 6 {
						switch line[0] {
						case 'V':
							if bytes.HasPrefix(line, []byte("VmRSS:")) {
								snap.vmRSSKb = int64(parseUintZeroAlloc(line[6:]))
							} else if bytes.HasPrefix(line, []byte("VmHWM:")) {
								snap.vmHWMKb = int64(parseUintZeroAlloc(line[6:]))
							}
						case 'R':
							if bytes.HasPrefix(line, []byte("RssAnon:")) {
								snap.rssAnonKb = int64(parseUintZeroAlloc(line[8:]))
							} else if bytes.HasPrefix(line, []byte("RssFile:")) {
								snap.rssFileKb = int64(parseUintZeroAlloc(line[8:]))
							}
						case 'v':
							if bytes.HasPrefix(line, []byte("voluntary_ctxt_switches:")) {
								snap.volCtxtSw = parseUintZeroAlloc(line[24:])
							}
						case 'n':
							if bytes.HasPrefix(line, []byte("nonvoluntary_ctxt_switches:")) {
								snap.nonvolCtxtSw = parseUintZeroAlloc(line[27:])
							}
						}
					}
					lineStart = i + 1
				}
			}
		}
	}

	// 3. Parse /proc/<pid>/io
	if p.ioFD >= 0 {
		n, err = syscall.Pread(p.ioFD, buf, 0)
		if err == nil && n > 0 {
			iodata := buf[:n]
			lineStart := 0
			for i := 0; i < len(iodata); i++ {
				if iodata[i] == '\n' {
					line := iodata[lineStart:i]
					if bytes.HasPrefix(line, []byte("rchar:")) {
						snap.rcharBytes = parseUintZeroAlloc(line[6:])
					} else if bytes.HasPrefix(line, []byte("wchar:")) {
						snap.wcharBytes = parseUintZeroAlloc(line[6:])
					} else if bytes.HasPrefix(line, []byte("syscr:")) {
						snap.syscr = parseUintZeroAlloc(line[6:])
					} else if bytes.HasPrefix(line, []byte("syscw:")) {
						snap.syscw = parseUintZeroAlloc(line[6:])
					} else if bytes.HasPrefix(line, []byte("read_bytes:")) {
						snap.readBytes = parseUintZeroAlloc(line[11:])
					} else if bytes.HasPrefix(line, []byte("write_bytes:")) {
						snap.writeBytes = parseUintZeroAlloc(line[12:])
					}
					lineStart = i + 1
				}
			}
		}
	}
	return true
}

// sumAllThreadsCtxtSwitches sums voluntary and nonvoluntary context switches across all OS threads
// of a process (/proc/<pid>/task/*/status) and reads Cpus_allowed_list. Run only at t=start and t=end.
func sumAllThreadsCtxtSwitches(pid int) (vol uint64, nonvol uint64, cpusAllowed string) {
	if pid <= 0 {
		return 0, 0, "N/A"
	}
	entries, err := os.ReadDir(fmt.Sprintf("/proc/%d/task", pid))
	if err != nil {
		return 0, 0, "N/A"
	}
	var buf [4096]byte
	for _, entry := range entries {
		statusPath := fmt.Sprintf("/proc/%d/task/%s/status", pid, entry.Name())
		fd, err := syscall.Open(statusPath, syscall.O_RDONLY, 0)
		if err != nil {
			continue
		}
		n, _ := syscall.Pread(fd, buf[:], 0)
		_ = syscall.Close(fd)
		if n <= 0 {
			continue
		}
		sdata := buf[:n]
		lineStart := 0
		for i := 0; i < len(sdata); i++ {
			if sdata[i] == '\n' {
				line := sdata[lineStart:i]
				if bytes.HasPrefix(line, []byte("voluntary_ctxt_switches:")) {
					vol += parseUintZeroAlloc(line[24:])
				} else if bytes.HasPrefix(line, []byte("nonvoluntary_ctxt_switches:")) {
					nonvol += parseUintZeroAlloc(line[27:])
				} else if cpusAllowed == "" && bytes.HasPrefix(line, []byte("Cpus_allowed_list:")) {
					cpusAllowed = strings.TrimSpace(string(line[18:]))
				}
				lineStart = i + 1
			}
		}
	}
	return vol, nonvol, cpusAllowed
}

// KernelNetSnap holds Linux kernel TCP stack counters from /proc/net/netstat
type KernelNetSnap struct {
	listenOverflows     uint64
	listenDrops         uint64
	tcpSynRetrans       uint64
	tcpTimeouts         uint64
	tcpReqQFullCookies  uint64
	tcpReqQFullDrop     uint64
	tcpTimeWaitOverflow uint64
	tcpAbortOnClose     uint64
	tcpAbortOnData      uint64
}

type NetstatReader struct {
	fd                  int
	idxListenOverflows  int
	idxListenDrops      int
	idxSynRetrans       int
	idxTimeouts         int
	idxReqQFullCookies  int
	idxReqQFullDrop     int
	idxTimeWaitOverflow int
	idxAbortOnClose     int
	idxAbortOnData      int
}

func openNetstatReader() *NetstatReader {
	fd, err := syscall.Open("/proc/net/netstat", syscall.O_RDONLY, 0)
	if err != nil {
		return nil
	}
	nr := &NetstatReader{
		fd:                  fd,
		idxListenOverflows:  -1,
		idxListenDrops:      -1,
		idxSynRetrans:       -1,
		idxTimeouts:         -1,
		idxReqQFullCookies:  -1,
		idxReqQFullDrop:     -1,
		idxTimeWaitOverflow: -1,
		idxAbortOnClose:     -1,
		idxAbortOnData:      -1,
	}
	var buf [8192]byte
	n, err := syscall.Pread(fd, buf[:], 0)
	if err == nil && n > 0 {
		lines := strings.Split(string(buf[:n]), "\n")
		for _, line := range lines {
			if strings.HasPrefix(line, "TcpExt:") {
				fields := strings.Fields(line)
				for i, name := range fields {
					switch name {
					case "ListenOverflows":
						nr.idxListenOverflows = i
					case "ListenDrops":
						nr.idxListenDrops = i
					case "TCPSynRetrans":
						nr.idxSynRetrans = i
					case "TCPTimeouts":
						nr.idxTimeouts = i
					case "TCPReqQFullDoCookies":
						nr.idxReqQFullCookies = i
					case "TCPReqQFullDrop":
						nr.idxReqQFullDrop = i
					case "TCPTimeWaitOverflow":
						nr.idxTimeWaitOverflow = i
					case "TCPAbortOnClose":
						nr.idxAbortOnClose = i
					case "TCPAbortOnData":
						nr.idxAbortOnData = i
					}
				}
				break
			}
		}
	}
	return nr
}

func (nr *NetstatReader) Close() {
	if nr != nil && nr.fd >= 0 {
		_ = syscall.Close(nr.fd)
	}
}

func (nr *NetstatReader) Sample(buf []byte, out *KernelNetSnap) {
	if nr == nil || nr.fd < 0 {
		return
	}
	n, err := syscall.Pread(nr.fd, buf, 0)
	if err != nil || n <= 0 {
		return
	}
	data := buf[:n]
	// Find the second "TcpExt:" line (the numeric values line)
	first := bytes.Index(data, []byte("TcpExt:"))
	if first < 0 {
		return
	}
	rest := data[first+7:]
	second := bytes.Index(rest, []byte("TcpExt:"))
	if second < 0 {
		return
	}
	valLine := rest[second:]
	endLine := bytes.IndexByte(valLine, '\n')
	if endLine > 0 {
		valLine = valLine[:endLine]
	}

	fieldIdx := 0
	i := 0
	for i < len(valLine) {
		for i < len(valLine) && valLine[i] == ' ' {
			i++
		}
		start := i
		for i < len(valLine) && valLine[i] != ' ' {
			i++
		}
		if start < i {
			if fieldIdx > 0 {
				switch fieldIdx {
				case nr.idxListenOverflows:
					out.listenOverflows = parseUintZeroAlloc(valLine[start:i])
				case nr.idxListenDrops:
					out.listenDrops = parseUintZeroAlloc(valLine[start:i])
				case nr.idxSynRetrans:
					out.tcpSynRetrans = parseUintZeroAlloc(valLine[start:i])
				case nr.idxTimeouts:
					out.tcpTimeouts = parseUintZeroAlloc(valLine[start:i])
				case nr.idxReqQFullCookies:
					out.tcpReqQFullCookies = parseUintZeroAlloc(valLine[start:i])
				case nr.idxReqQFullDrop:
					out.tcpReqQFullDrop = parseUintZeroAlloc(valLine[start:i])
				case nr.idxTimeWaitOverflow:
					out.tcpTimeWaitOverflow = parseUintZeroAlloc(valLine[start:i])
				case nr.idxAbortOnClose:
					out.tcpAbortOnClose = parseUintZeroAlloc(valLine[start:i])
				case nr.idxAbortOnData:
					out.tcpAbortOnData = parseUintZeroAlloc(valLine[start:i])
				}
			}
			fieldIdx++
		}
	}
}

// pinProcessCPUs pins all OS threads of pid to the specified CPU core bitmask via sched_setaffinity.
func pinProcessCPUs(pid int, mask uint64) error {
	var cpuMask [16]uint64
	cpuMask[0] = mask
	entries, err := os.ReadDir(fmt.Sprintf("/proc/%d/task", pid))
	if err != nil {
		_, _, errno := syscall.Syscall(syscall.SYS_SCHED_SETAFFINITY, uintptr(pid), unsafe.Sizeof(cpuMask), uintptr(unsafe.Pointer(&cpuMask[0])))
		if errno != 0 {
			return errno
		}
		return nil
	}
	for _, e := range entries {
		tid, err := strconv.Atoi(e.Name())
		if err == nil && tid > 0 {
			_, _, _ = syscall.Syscall(syscall.SYS_SCHED_SETAFFINITY, uintptr(tid), unsafe.Sizeof(cpuMask), uintptr(unsafe.Pointer(&cpuMask[0])))
		}
	}
	return nil
}

type TimelinePoint struct {
	elapsedSec   float64
	activeVUs    int64
	rps          float64
	errPerSec    float64
	srvUserCPU   float64
	srvSysCPU    float64
	srvTotalCPU  float64
	srvRSSMiB    float64
	srvAnonMiB   float64
	srvMmapMiB   float64
	srvWriteKBps float64
	genTotalCPU  float64
	genRSSMiB    float64
	synRetrans   uint64
	listenDrops  uint64
}

func main() {
	usersFlag := flag.Int("users", 1000, "Number of concurrent virtual users (VUs)")
	durationFlag := flag.Duration("duration", 20*time.Second, "Steady-state plateau test duration")
	rampUpFlag := flag.Duration("rampup", 10*time.Second, "Ramp-up duration to connect VUs gradually")
	serverPIDFlag := flag.Int("pid", 0, "Server PID to monitor per-process CPU, RAM, I/O, and context switches")
	tokensPathFlag := flag.String("tokens", "seed/tokens.json", "Path to seed/tokens.json")
	fastModeFlag := flag.Bool("fast", false, "High-throughput stress mode (5-20ms jitter); default false (3-8s official k6 think time)")
	hostPortFlag := flag.String("target", "127.0.0.1:3000", "Target server host:port")
	pinCPUsFlag := flag.Bool("pin-cpus", true, "Isolate CPU cores via sched_setaffinity (Server on Physical Core 0, Loadgen on remaining cores)")
	csvPathFlag := flag.String("csv", "", "Optional file path to export second-by-second per-process telemetry as CSV")
	tlsFlag := flag.Bool("tls", false, "Enable TLS/HTTPS (automatically enabled if target port is 443)")
	hostHeaderFlag := flag.String("host", "", "HTTP Host header override (defaults to target hostname)")
	flag.Parse()

	selfPID := os.Getpid()
	srvPID := *serverPIDFlag

	// Optional hardware core isolation (cgroups/sched_setaffinity equivalent)
	numCPUs := runtime.NumCPU()
	if *pinCPUsFlag && srvPID > 0 && numCPUs >= 4 {
		// On 8-thread Intel i5-1135G7: Core 0 = CPU 0 & CPU 4; Cores 1..3 = CPUs 1,2,3,5,6,7.
		// Pin server to Physical Core 0 (CPU 0,4) and loadgen_pro to Cores 1..3 (CPUs 1,2,3,5,6,7)
		var srvMask, genMask uint64
		if numCPUs == 8 {
			srvMask = (1 << 0) | (1 << 4)
			genMask = (1 << 1) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7)
		} else {
			srvMask = 1 << 0
			for c := 1; c < numCPUs; c++ {
				genMask |= (1 << uint(c))
			}
		}
		_ = pinProcessCPUs(srvPID, srvMask)
		_ = pinProcessCPUs(selfPID, genMask)
	}

	// 1. Load test tokens
	tokensPath := *tokensPathFlag
	if _, err := os.Stat(tokensPath); err != nil {
		alt := filepath.Join("..", "..", tokensPath)
		if _, err2 := os.Stat(alt); err2 == nil {
			tokensPath = alt
		} else {
			tokensPath = filepath.Join("..", tokensPath)
		}
	}
	tokensBytes, err := os.ReadFile(tokensPath)
	if err != nil {
		fmt.Printf("Error loading tokens from %s: %v\n", tokensPath, err)
		os.Exit(1)
	}
	var tokens []TokenUser
	if err := json.Unmarshal(tokensBytes, &tokens); err != nil || len(tokens) == 0 {
		fmt.Printf("Error parsing tokens: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("Loaded %d seed tokens.\n", len(tokens))

	// 2. Prepare 25 local loopback IPs (supports up to ~700,000 concurrent sockets)
	numIPs := 25
	localIPs := make([]net.IP, numIPs)
	for i := 0; i < numIPs; i++ {
		localIPs[i] = net.IPv4(127, 0, 0, byte(i+1))
	}

	// Atomic histograms and counters
	var (
		overallHist   Histogram
		feedHist      Histogram
		postHist      Histogram
		likeHist      Histogram
		createHist    Histogram
		totalReqs     atomic.Uint64
		errReqs       atomic.Uint64
		statusCounts  [600]atomic.Uint64
		activeSockets atomic.Int64
	)

	// 3. Persistent-FD Zero-Allocation Process & Kernel Monitor
	srvFDs := openProcFDs(srvPID)
	genFDs := openProcFDs(selfPID)
	netReader := openNetstatReader()
	defer srvFDs.Close()
	defer genFDs.Close()
	defer netReader.Close()

	var (
		srvStartSnap, srvEndSnap, srvPeakSnap ProcSnapshot
		genStartSnap, genEndSnap, genPeakSnap ProcSnapshot
		netStartSnap, netEndSnap              KernelNetSnap
		srvPeakTotalCPU, srvPeakUserCPU, srvPeakSysCPU float64
		genPeakTotalCPU, genPeakUserCPU, genPeakSysCPU float64
		timeline = make([]TimelinePoint, 0, 256)
	)

	var monBuf [8192]byte
	if srvFDs != nil {
		sampleProcFast(srvFDs, monBuf[:], &srvStartSnap)
		srvStartSnap.volCtxtSw, srvStartSnap.nonvolCtxtSw, srvStartSnap.cpusAllowed = sumAllThreadsCtxtSwitches(srvPID)
		srvPeakSnap = srvStartSnap
	}
	if genFDs != nil {
		sampleProcFast(genFDs, monBuf[:], &genStartSnap)
		genStartSnap.volCtxtSw, genStartSnap.nonvolCtxtSw, genStartSnap.cpusAllowed = sumAllThreadsCtxtSwitches(selfPID)
		genPeakSnap = genStartSnap
	}
	if netReader != nil {
		netReader.Sample(monBuf[:], &netStartSnap)
	}

	stopMonitor := make(chan struct{})
	monitorDone := make(chan struct{})
	startTestTime := time.Now()

	go func() {
		defer close(monitorDone)
		ticker := time.NewTicker(1000 * time.Millisecond)
		defer ticker.Stop()

		prevTime := startTestTime
		prevSrv := srvStartSnap
		prevGen := genStartSnap
		var prevReqs, prevErrs uint64
		var curSrv, curGen ProcSnapshot
		var curNet KernelNetSnap

		for {
			select {
			case <-stopMonitor:
				return
			case t := <-ticker.C:
				deltaSec := t.Sub(prevTime).Seconds()
				if deltaSec <= 0 {
					continue
				}
				curReqs := totalReqs.Load()
				curErrs := errReqs.Load()
				rps := float64(curReqs-prevReqs) / deltaSec
				eps := float64(curErrs-prevErrs) / deltaSec

				var srvUsrPct, srvSysPct, srvTotPct, srvWriteKBps float64
				if srvFDs != nil && sampleProcFast(srvFDs, monBuf[:], &curSrv) {
					srvUsrPct = float64(curSrv.utimeTicks-prevSrv.utimeTicks) / deltaSec
					srvSysPct = float64(curSrv.stimeTicks-prevSrv.stimeTicks) / deltaSec
					srvTotPct = srvUsrPct + srvSysPct
					srvWriteKBps = float64(curSrv.writeBytes-prevSrv.writeBytes) / 1024.0 / deltaSec

					if srvTotPct > srvPeakTotalCPU {
						srvPeakTotalCPU = srvTotPct
					}
					if srvUsrPct > srvPeakUserCPU {
						srvPeakUserCPU = srvUsrPct
					}
					if srvSysPct > srvPeakSysCPU {
						srvPeakSysCPU = srvSysPct
					}
					if curSrv.vmRSSKb > srvPeakSnap.vmRSSKb {
						srvPeakSnap.vmRSSKb = curSrv.vmRSSKb
						srvPeakSnap.rssAnonKb = curSrv.rssAnonKb
						srvPeakSnap.rssFileKb = curSrv.rssFileKb
					}
					if curSrv.vmHWMKb > srvPeakSnap.vmHWMKb {
						srvPeakSnap.vmHWMKb = curSrv.vmHWMKb
					}
					prevSrv = curSrv
				}

				var genUsrPct, genSysPct, genTotPct float64
				if genFDs != nil && sampleProcFast(genFDs, monBuf[:], &curGen) {
					genUsrPct = float64(curGen.utimeTicks-prevGen.utimeTicks) / deltaSec
					genSysPct = float64(curGen.stimeTicks-prevGen.stimeTicks) / deltaSec
					genTotPct = genUsrPct + genSysPct

					if genTotPct > genPeakTotalCPU {
						genPeakTotalCPU = genTotPct
					}
					if genUsrPct > genPeakUserCPU {
						genPeakUserCPU = genUsrPct
					}
					if genSysPct > genPeakSysCPU {
						genPeakSysCPU = genSysPct
					}
					if curGen.vmRSSKb > genPeakSnap.vmRSSKb {
						genPeakSnap.vmRSSKb = curGen.vmRSSKb
						genPeakSnap.rssAnonKb = curGen.rssAnonKb
						genPeakSnap.rssFileKb = curGen.rssFileKb
					}
					if curGen.vmHWMKb > genPeakSnap.vmHWMKb {
						genPeakSnap.vmHWMKb = curGen.vmHWMKb
					}
					prevGen = curGen
				}

				if netReader != nil {
					netReader.Sample(monBuf[:], &curNet)
				}

				if len(timeline) < cap(timeline) {
					timeline = append(timeline, TimelinePoint{
						elapsedSec:   t.Sub(startTestTime).Seconds(),
						activeVUs:    activeSockets.Load(),
						rps:          rps,
						errPerSec:    eps,
						srvUserCPU:   srvUsrPct,
						srvSysCPU:    srvSysPct,
						srvTotalCPU:  srvTotPct,
						srvRSSMiB:    float64(curSrv.vmRSSKb) / 1024.0,
						srvAnonMiB:   float64(curSrv.rssAnonKb) / 1024.0,
						srvMmapMiB:   float64(curSrv.rssFileKb) / 1024.0,
						srvWriteKBps: srvWriteKBps,
						genTotalCPU:  genTotPct,
						genRSSMiB:    float64(curGen.vmRSSKb) / 1024.0,
						synRetrans:   curNet.tcpSynRetrans - netStartSnap.tcpSynRetrans,
						listenDrops:  curNet.listenDrops - netStartSnap.listenDrops,
					})
				}

				prevTime = t
				prevReqs = curReqs
				prevErrs = curErrs
			}
		}
	}()

	targetAddr := *hostPortFlag
	var hostHeader string
	isTLS := *tlsFlag
	if strings.Contains(targetAddr, ":") {
		h, p, err := net.SplitHostPort(targetAddr)
		if err == nil {
			hostHeader = h
			if p == "443" {
				isTLS = true
			}
		} else {
			hostHeader = targetAddr
		}
	} else {
		hostHeader = targetAddr
	}
	if *hostHeaderFlag != "" {
		hostHeader = *hostHeaderFlag
	}
	isLocal := strings.HasPrefix(hostHeader, "127.") || hostHeader == "localhost"

	numVUs := *usersFlag
	rampUp := *rampUpFlag
	steadyDuration := *durationFlag

	fmt.Printf("\nStarting benchmark with %d concurrent VUs...\n", numVUs)
	fmt.Printf("Target: %s (Host: %s, TLS: %v, Local: %v)\n", targetAddr, hostHeader, isTLS, isLocal)
	fmt.Printf("Connection ramp-up: %v (~%.0f VUs/s)\n", rampUp, float64(numVUs)/rampUp.Seconds())
	fmt.Printf("Steady-state plateau: %v\n", steadyDuration)
	if srvPID > 0 {
		fmt.Printf("Process Telemetry: Server PID=%d (CPUs: %s) | Loadgen PID=%d (CPUs: %s)\n",
			srvPID, srvStartSnap.cpusAllowed, selfPID, genStartSnap.cpusAllowed)
	}
	if *fastModeFlag {
		fmt.Println("Mode: High-Throughput Stress (5-20ms jitter)")
	} else {
		fmt.Println("Mode: Official k6 User Loop Simulation (3-8s / 5-15s think time)")
	}

	testDeadline := startTestTime.Add(rampUp + steadyDuration)
	var wg sync.WaitGroup

	batchSize := numVUs / int(rampUp/(10*time.Millisecond))
	if batchSize < 1 {
		batchSize = 1
	}
	if batchSize > 250 {
		batchSize = 250
	}
	batchDelay := rampUp / time.Duration(numVUs/batchSize+1)
	if batchDelay < 5*time.Millisecond {
		batchDelay = 5 * time.Millisecond
	}

	// Prepare 25 pre-allocated net.Dialers for local loopback IPs (if testing local server)
	dialers := make([]*net.Dialer, numIPs)
	for i := 0; i < numIPs; i++ {
		dialers[i] = &net.Dialer{
			LocalAddr: &net.TCPAddr{IP: localIPs[i]},
			Timeout:   10 * time.Second,
			KeepAlive: 65 * time.Second,
		}
	}
	remoteDialer := &net.Dialer{
		Timeout:   10 * time.Second,
		KeepAlive: 65 * time.Second,
	}

	// Offload net.Dialer.Dial onto a small pool of dialer goroutines so that the
	// 180,000+ VU goroutines never execute the deep net.Dialer stack and keep their
	// minimum 2 KB Go stack (saving ~1.1 GiB of RAM).
	type dialReq struct {
		ipIdx int
		reply chan net.Conn
	}
	dialCh := make(chan dialReq, 512)
	for w := 0; w < 64; w++ {
		go func() {
			for req := range dialCh {
				var c net.Conn
				var err error
				if isLocal {
					c, err = dialers[req.ipIdx].Dial("tcp", targetAddr)
				} else {
					c, err = remoteDialer.Dial("tcp", targetAddr)
				}
				if err != nil {
					req.reply <- nil
					continue
				}
				if tc, ok := c.(*net.TCPConn); ok {
					_ = tc.SetNoDelay(true)
					_ = tc.SetKeepAlive(true)
					_ = tc.SetKeepAlivePeriod(65 * time.Second)
				}
				if isTLS {
					tlsConn := tls.Client(c, &tls.Config{
						ServerName: hostHeader,
					})
					if err := tlsConn.Handshake(); err != nil {
						c.Close()
						req.reply <- nil
						continue
					}
					c = tlsConn
				}
				req.reply <- c
			}
		}()
	}

	type ioScratch struct {
		reader  *bufio.Reader
		writer  *bufio.Writer
		body    []byte
		postBuf [256]byte
		pathBuf [64]byte
		replyCh chan net.Conn
	}
	ioPool := sync.Pool{
		New: func() any {
			return &ioScratch{
				reader:  bufio.NewReaderSize(nil, 8192),
				writer:  bufio.NewWriterSize(nil, 1024),
				body:    make([]byte, 16384),
				replyCh: make(chan net.Conn, 1),
			}
		},
	}

	dialViaPool := func(ipIdx int) net.Conn {
		sc := ioPool.Get().(*ioScratch)
		dialCh <- dialReq{ipIdx: ipIdx, reply: sc.replyCh}
		c := <-sc.replyCh
		ioPool.Put(sc)
		if c == nil {
			errReqs.Add(1)
			statusCounts[0].Add(1)
			return nil
		}
		activeSockets.Add(1)
		return c
	}

	doReqShared := func(conn *net.Conn, ipIdx int, method, path string, authToken string, jsonContentType bool, body []byte) (int, int, time.Duration) {
		for attempt := 0; attempt < 2; attempt++ {
			if *conn == nil {
				*conn = dialViaPool(ipIdx)
				if *conn == nil {
					return 0, 0, 0
				}
			}
			c := *conn

			sc := ioPool.Get().(*ioScratch)
			sc.reader.Reset(c)
			sc.writer.Reset(c)

			t0 := time.Now()
			_ = c.SetDeadline(t0.Add(10 * time.Second))

			_, _ = sc.writer.WriteString(method)
			_ = sc.writer.WriteByte(' ')
			_, _ = sc.writer.WriteString(path)
			_, _ = sc.writer.WriteString(" HTTP/1.1\r\nHost: ")
			_, _ = sc.writer.WriteString(hostHeader)
			_, _ = sc.writer.WriteString("\r\nConnection: keep-alive\r\n")
			if authToken != "" {
				_, _ = sc.writer.WriteString("Authorization: Bearer ")
				_, _ = sc.writer.WriteString(authToken)
				_, _ = sc.writer.WriteString("\r\n")
			}
			if jsonContentType {
				_, _ = sc.writer.WriteString("Content-Type: application/json\r\n")
			}
			if len(body) > 0 {
				_, _ = sc.writer.WriteString("Content-Length: ")
				var lenBuf [20]byte
				_, _ = sc.writer.Write(strconv.AppendInt(lenBuf[:0], int64(len(body)), 10))
				_, _ = sc.writer.WriteString("\r\n\r\n")
				_, _ = sc.writer.Write(body)
			} else {
				_, _ = sc.writer.WriteString("\r\n")
			}
			if err := sc.writer.Flush(); err != nil {
				sc.reader.Reset(nil)
				sc.writer.Reset(nil)
				ioPool.Put(sc)
				c.Close()
				activeSockets.Add(-1)
				*conn = nil
				continue
			}

			statusLine, err := sc.reader.ReadSlice('\n')
			if err != nil {
				sc.reader.Reset(nil)
				sc.writer.Reset(nil)
				ioPool.Put(sc)
				c.Close()
				activeSockets.Add(-1)
				*conn = nil
				continue
			}
			statusCode := 0
			if sp1 := bytes.IndexByte(statusLine, ' '); sp1 > 0 && sp1+4 <= len(statusLine) {
				statusCode = int(parseUintZeroAlloc(statusLine[sp1+1 : sp1+4]))
			}

			contentLen := 0
			for {
				line, err := sc.reader.ReadSlice('\n')
				if err != nil {
					break
				}
				if len(line) <= 2 && (line[0] == '\r' || line[0] == '\n') {
					break
				}
				if len(line) > 15 && (line[0] == 'C' || line[0] == 'c') && (line[8] == 'L' || line[8] == 'l') {
					contentLen = int(parseUintZeroAlloc(line[15:]))
				}
			}

			firstID := 0
			if contentLen > 0 {
				if cap(sc.body) < contentLen {
					sc.body = make([]byte, contentLen)
				}
				respBody := sc.body[:contentLen]
				if _, err := io.ReadFull(sc.reader, respBody); err != nil {
					sc.reader.Reset(nil)
					sc.writer.Reset(nil)
					ioPool.Put(sc)
					c.Close()
					activeSockets.Add(-1)
					*conn = nil
					continue
				}
				if idx := bytes.Index(respBody, []byte(`"id":`)); idx != -1 {
					firstID = int(parseUintZeroAlloc(respBody[idx+5:]))
				}
			}

			lat := time.Since(t0)
			sc.reader.Reset(nil)
			sc.writer.Reset(nil)
			ioPool.Put(sc)
			return statusCode, firstID, lat
		}
		return 0, 0, 0
	}

	for i := 0; i < numVUs; i++ {
		vuID := i + 1
		ipIdx := i % numIPs
		user := tokens[i%len(tokens)]

		wg.Add(1)
		go func(id int, lipIdx int, u TokenUser) {
			defer wg.Done()

			// Zero-allocation 8-byte xorshift64 PRNG on stack (saves ~900 MiB vs rand.NewSource)
			rngState := uint64(time.Now().UnixNano()) ^ (uint64(id) * 0x9E3779B97F4A7C15)
			if rngState == 0 {
				rngState = 1
			}
			nextRand := func(n int) int {
				rngState ^= rngState << 13
				rngState ^= rngState >> 7
				rngState ^= rngState << 17
				return int(rngState % uint64(n))
			}

			conn := dialViaPool(lipIdx)
			if conn == nil {
				return
			}
			defer func() {
				if conn != nil {
					conn.Close()
					activeSockets.Add(-1)
				}
			}()

			// Stagger first request across the think-time window
			if *fastModeFlag {
				time.Sleep(time.Duration(nextRand(20)) * time.Millisecond)
			} else {
				time.Sleep(time.Duration(nextRand(3000)) * time.Millisecond)
			}

			for time.Now().Before(testDeadline) {
				// 1. GET /feed
				code, parsedID, lat := doReqShared(&conn, lipIdx, "GET", "/feed", "", false, nil)
				totalReqs.Add(1)
				if code >= 0 && code < 600 {
					statusCounts[code].Add(1)
				}
				if code != 200 {
					errReqs.Add(1)
					if *fastModeFlag {
						time.Sleep(10 * time.Millisecond)
					} else {
						time.Sleep(1 * time.Second)
					}
					continue
				}
				us := lat.Microseconds()
				overallHist.Record(us)
				feedHist.Record(us)

				postID := 500000 - nextRand(20)
				if parsedID > 0 {
					postID = parsedID
				}

				if *fastModeFlag {
					time.Sleep(time.Duration(5+nextRand(15)) * time.Millisecond)
				} else {
					time.Sleep(time.Duration(3000+nextRand(5000)) * time.Millisecond)
				}

				if !time.Now().Before(testDeadline) {
					break
				}

				// 2. GET /posts/:id
				var pathBuf [48]byte
				postPath := string(strconv.AppendInt(append(pathBuf[:0], "/posts/"...), int64(postID), 10))
				code, _, lat = doReqShared(&conn, lipIdx, "GET", postPath, "", false, nil)
				totalReqs.Add(1)
				if code >= 0 && code < 600 {
					statusCounts[code].Add(1)
				}
				if code == 200 {
					us = lat.Microseconds()
					overallHist.Record(us)
					postHist.Record(us)
				} else {
					errReqs.Add(1)
				}

				// 3. 15% chance: POST /posts/:id/like
				if nextRand(100) < 15 {
					if *fastModeFlag {
						time.Sleep(time.Duration(3+nextRand(10)) * time.Millisecond)
					} else {
						time.Sleep(time.Duration(3000+nextRand(5000)) * time.Millisecond)
					}

					likePath := string(append(strconv.AppendInt(append(pathBuf[:0], "/posts/"...), int64(postID), 10), "/like"...))
					code, _, lat = doReqShared(&conn, lipIdx, "POST", likePath, u.Token, false, nil)
					totalReqs.Add(1)
					if code >= 0 && code < 600 {
						statusCounts[code].Add(1)
					}
					if code == 200 || code == 201 {
						us := lat.Microseconds()
						overallHist.Record(us)
						likeHist.Record(us)
					} else {
						errReqs.Add(1)
					}
				}

				// 4. 2% chance: POST /posts
				if nextRand(100) < 2 {
					if *fastModeFlag {
						time.Sleep(time.Duration(3+nextRand(10)) * time.Millisecond)
					} else {
						time.Sleep(time.Duration(3000+nextRand(5000)) * time.Millisecond)
					}

					var bodyBuf [160]byte
					b := append(bodyBuf[:0], `{"body":"`...)
					b = append(b, u.Username...)
					b = append(b, ` says hello (VU `...)
					b = strconv.AppendInt(b, int64(id), 10)
					b = append(b, ` )"}`...)
					code, _, lat = doReqShared(&conn, lipIdx, "POST", "/posts", u.Token, true, b)
					totalReqs.Add(1)
					if code >= 0 && code < 600 {
						statusCounts[code].Add(1)
					}
					if code == 201 {
						us := lat.Microseconds()
						overallHist.Record(us)
						createHist.Record(us)
					} else {
						errReqs.Add(1)
					}
				}

				// End-of-loop think time
				if *fastModeFlag {
					time.Sleep(time.Duration(5+nextRand(20)) * time.Millisecond)
				} else {
					time.Sleep(time.Duration(5000+nextRand(10000)) * time.Millisecond)
				}
			}
		}(vuID, ipIdx, user)

		if (i+1)%batchSize == 0 {
			time.Sleep(batchDelay)
		}
	}

	wg.Wait()
	totalDuration := time.Since(startTestTime)
	close(stopMonitor)
	<-monitorDone

	// Capture final /proc snapshots before closing
	if srvFDs != nil {
		sampleProcFast(srvFDs, monBuf[:], &srvEndSnap)
		srvEndSnap.volCtxtSw, srvEndSnap.nonvolCtxtSw, srvEndSnap.cpusAllowed = sumAllThreadsCtxtSwitches(srvPID)
		if srvEndSnap.vmRSSKb > srvPeakSnap.vmRSSKb {
			srvPeakSnap.vmRSSKb = srvEndSnap.vmRSSKb
			srvPeakSnap.rssAnonKb = srvEndSnap.rssAnonKb
			srvPeakSnap.rssFileKb = srvEndSnap.rssFileKb
		}
		if srvEndSnap.vmHWMKb > srvPeakSnap.vmHWMKb {
			srvPeakSnap.vmHWMKb = srvEndSnap.vmHWMKb
		}
	}
	if genFDs != nil {
		sampleProcFast(genFDs, monBuf[:], &genEndSnap)
		genEndSnap.volCtxtSw, genEndSnap.nonvolCtxtSw, genEndSnap.cpusAllowed = sumAllThreadsCtxtSwitches(selfPID)
		if genEndSnap.vmRSSKb > genPeakSnap.vmRSSKb {
			genPeakSnap.vmRSSKb = genEndSnap.vmRSSKb
			genPeakSnap.rssAnonKb = genEndSnap.rssAnonKb
			genPeakSnap.rssFileKb = genEndSnap.rssFileKb
		}
		if genEndSnap.vmHWMKb > genPeakSnap.vmHWMKb {
			genPeakSnap.vmHWMKb = genEndSnap.vmHWMKb
		}
	}
	if netReader != nil {
		netReader.Sample(monBuf[:], &netEndSnap)
	}

	// Print benchmark report
	tot := totalReqs.Load()
	errs := errReqs.Load()
	errPct := 0.0
	if tot > 0 {
		errPct = float64(errs) / float64(tot) * 100.0
	}
	p95 := overallHist.Percentile(0.95)
	p99 := overallHist.Percentile(0.99)
	passed := p95 < 500*time.Millisecond && p99 < 1000*time.Millisecond && errPct < 1.0

	fmt.Println("\n========================================================================")
	fmt.Println("BENCHMARK RESULTS SUMMARY")
	fmt.Println("========================================================================")
	fmt.Printf("Target Concurrent VUs:  %d\n", numVUs)
	fmt.Printf("Total Elapsed Time:     %.2f s\n", totalDuration.Seconds())
	fmt.Printf("Total Requests:         %d\n", tot)
	fmt.Printf("Average Throughput:     %.2f req/s\n", float64(tot)/totalDuration.Seconds())
	fmt.Printf("Failed Requests:        %d (%.3f%%)\n", errs, errPct)

	if srvPID > 0 {
		elSec := totalDuration.Seconds()
		srvAvgUser := float64(srvEndSnap.utimeTicks-srvStartSnap.utimeTicks) / elSec
		srvAvgSys := float64(srvEndSnap.stimeTicks-srvStartSnap.stimeTicks) / elSec
		srvAvgTot := srvAvgUser + srvAvgSys

		genAvgUser := float64(genEndSnap.utimeTicks-genStartSnap.utimeTicks) / elSec
		genAvgSys := float64(genEndSnap.stimeTicks-genStartSnap.stimeTicks) / elSec
		genAvgTot := genAvgUser + genAvgSys

		fmt.Println("\n--- PER-PROCESS TELEMETRY MAP (Zero-Alloc /proc Persistent FDs) ---")
		fmt.Printf("%-34s | %-24s | %-24s\n", "Metric", fmt.Sprintf("Server (PID %d)", srvPID), fmt.Sprintf("Loadgen (PID %d)", selfPID))
		fmt.Println(strings.Repeat("-", 88))
		fmt.Printf("%-34s | %-24s | %-24s\n", "Allowed CPU Cores (Affinity)", srvEndSnap.cpusAllowed, genEndSnap.cpusAllowed)
		fmt.Printf("%-34s | %-24d | %-24d\n", "Last Executed Logical CPU", srvEndSnap.lastCPU, genEndSnap.lastCPU)
		fmt.Printf("%-34s | %-24d | %-24d\n", "Active OS Threads", srvEndSnap.numThreads, genEndSnap.numThreads)
		fmt.Printf("%-34s | %6.1f%% (Peak: %5.1f%%)  | %6.1f%% (Peak: %5.1f%%)\n",
			"Total CPU Usage", srvAvgTot, srvPeakTotalCPU, genAvgTot, genPeakTotalCPU)
		fmt.Printf("%-34s | %6.1f%% (Peak: %5.1f%%)  | %6.1f%% (Peak: %5.1f%%)\n",
			"  - User Space CPU (utime)", srvAvgUser, srvPeakUserCPU, genAvgUser, genPeakUserCPU)
		fmt.Printf("%-34s | %6.1f%% (Peak: %5.1f%%)  | %6.1f%% (Peak: %5.1f%%)\n",
			"  - Kernel Syscall CPU (stime)", srvAvgSys, srvPeakSysCPU, genAvgSys, genPeakSysCPU)
		fmt.Printf("%-34s | %8.2f MiB             | %8.2f MiB\n",
			"Peak Physical RAM (VmRSS)", float64(srvPeakSnap.vmRSSKb)/1024.0, float64(genPeakSnap.vmRSSKb)/1024.0)
		fmt.Printf("%-34s | %8.2f MiB             | %8.2f MiB\n",
			"  - Anonymous Heap/Stack (RssAnon)", float64(srvPeakSnap.rssAnonKb)/1024.0, float64(genPeakSnap.rssAnonKb)/1024.0)
		fmt.Printf("%-34s | %8.2f MiB             | %8.2f MiB\n",
			"  - Mapped Page Cache (RssFile)", float64(srvPeakSnap.rssFileKb)/1024.0, float64(genPeakSnap.rssFileKb)/1024.0)
		fmt.Printf("%-34s | %8.2f MiB             | %8.2f MiB\n",
			"Kernel High-Water Mark (VmHWM)", float64(srvPeakSnap.vmHWMKb)/1024.0, float64(genPeakSnap.vmHWMKb)/1024.0)
		fmt.Printf("%-34s | %-24d | %-24d\n",
			"Voluntary Context Switches", srvEndSnap.volCtxtSw-srvStartSnap.volCtxtSw, genEndSnap.volCtxtSw-genStartSnap.volCtxtSw)
		fmt.Printf("%-34s | %-24d | %-24d\n",
			"Involuntary Ctxt Switches (Preempt)", srvEndSnap.nonvolCtxtSw-srvStartSnap.nonvolCtxtSw, genEndSnap.nonvolCtxtSw-genStartSnap.nonvolCtxtSw)
		fmt.Printf("%-34s | %8.2f MiB             | %8.2f MiB\n",
			"Physical SSD Writes (write_bytes)", float64(srvEndSnap.writeBytes-srvStartSnap.writeBytes)/(1024*1024), float64(genEndSnap.writeBytes-genStartSnap.writeBytes)/(1024*1024))
		fmt.Printf("%-34s | %8.2f MiB             | %8.2f MiB\n",
			"Physical SSD Reads (read_bytes)", float64(srvEndSnap.readBytes-srvStartSnap.readBytes)/(1024*1024), float64(genEndSnap.readBytes-genStartSnap.readBytes)/(1024*1024))
		fmt.Printf("%-34s | %8.2f MiB             | %8.2f MiB\n",
			"Logical Syscall Writes (wchar)", float64(srvEndSnap.wcharBytes-srvStartSnap.wcharBytes)/(1024*1024), float64(genEndSnap.wcharBytes-genStartSnap.wcharBytes)/(1024*1024))
		fmt.Printf("%-34s | %-24d | %-24d\n",
			"Write Syscalls (syscw)", srvEndSnap.syscw-srvStartSnap.syscw, genEndSnap.syscw-genStartSnap.syscw)
	}

	if netReader != nil {
		fmt.Println("\n--- LINUX KERNEL TCP STACK TELEMETRY (/proc/net/netstat Deltas) ---")
		fmt.Printf("ListenOverflows (Accept Queue Full): %d\n", netEndSnap.listenOverflows-netStartSnap.listenOverflows)
		fmt.Printf("ListenDrops (Total Listen Drops):    %d\n", netEndSnap.listenDrops-netStartSnap.listenDrops)
		fmt.Printf("TCPReqQFullDoCookies (SYN Cookies):  %d\n", netEndSnap.tcpReqQFullCookies-netStartSnap.tcpReqQFullCookies)
		fmt.Printf("TCPReqQFullDrop (SYN Queue Drops):   %d\n", netEndSnap.tcpReqQFullDrop-netStartSnap.tcpReqQFullDrop)
		fmt.Printf("TCPSynRetrans (SYN Retransmits):     %d\n", netEndSnap.tcpSynRetrans-netStartSnap.tcpSynRetrans)
		fmt.Printf("TCPTimeouts (Retransmit Timeouts):   %d\n", netEndSnap.tcpTimeouts-netStartSnap.tcpTimeouts)
		fmt.Printf("TCPTimeWaitOverflow (TW Bucket Cap): %d\n", netEndSnap.tcpTimeWaitOverflow-netStartSnap.tcpTimeWaitOverflow)
	}

	if len(timeline) > 0 && srvPID > 0 {
		fmt.Println("\n--- TIME-SERIES TELEMETRY EVOLUTION (Sampled During Test) ---")
		fmt.Printf("%6s | %9s | %9s | %15s | %18s | %11s | %8s | %8s\n",
			"Time", "ActiveVUs", "RPS", "SrvCPU(Usr/Sys)", "SrvRAM(Anon/Mmap)", "DiskWrite", "GenCPU", "SynRetr")
		fmt.Println(strings.Repeat("-", 98))
		step := 1
		if len(timeline) > 20 {
			step = len(timeline) / 15
		}
		for i := 0; i < len(timeline); i += step {
			pt := timeline[i]
			fmt.Printf("%5.0fs | %9d | %9.0f | %5.1f%%(%3.0f/%3.0f) | %6.1fM(%4.0f/%4.0f) | %7.1f KB/s | %7.1f%% | %8d\n",
				pt.elapsedSec, pt.activeVUs, pt.rps,
				pt.srvTotalCPU, pt.srvUserCPU, pt.srvSysCPU,
				pt.srvRSSMiB, pt.srvAnonMiB, pt.srvMmapMiB,
				pt.srvWriteKBps, pt.genTotalCPU, pt.synRetrans)
		}
	}

	if *csvPathFlag != "" && len(timeline) > 0 {
		if f, err := os.Create(*csvPathFlag); err == nil {
			_, _ = fmt.Fprintln(f, "elapsed_s,active_vus,rps,err_per_s,srv_user_cpu,srv_sys_cpu,srv_total_cpu,srv_rss_mib,srv_anon_mib,srv_mmap_mib,srv_write_kbps,gen_total_cpu,gen_rss_mib,syn_retrans,listen_drops")
			for _, pt := range timeline {
				_, _ = fmt.Fprintf(f, "%.2f,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d\n",
					pt.elapsedSec, pt.activeVUs, pt.rps, pt.errPerSec,
					pt.srvUserCPU, pt.srvSysCPU, pt.srvTotalCPU,
					pt.srvRSSMiB, pt.srvAnonMiB, pt.srvMmapMiB,
					pt.srvWriteKBps, pt.genTotalCPU, pt.genRSSMiB,
					pt.synRetrans, pt.listenDrops)
			}
			_ = f.Close()
			fmt.Printf("\nExported %d time-series rows to CSV: %s\n", len(timeline), *csvPathFlag)
		}
	}

	fmt.Println("\n--- OVERALL LATENCY DISTRIBUTION ---")
	fmt.Printf("Average: %v\n", overallHist.Avg())
	fmt.Printf("p50:     %v\n", overallHist.Percentile(0.50))
	fmt.Printf("p90:     %v\n", overallHist.Percentile(0.90))
	fmt.Printf("p95:     %v\n", p95)
	fmt.Printf("p99:     %v\n", p99)
	fmt.Printf("Max:     %v\n", overallHist.Max())

	fmt.Println("\n--- LATENCY BY ENDPOINT ---")
	printEp := func(name string, h *Histogram) {
		fmt.Printf("[%s] Count: %d | Avg: %v | p95: %v | p99: %v\n",
			name, h.total.Load(), h.Avg(), h.Percentile(0.95), h.Percentile(0.99))
	}
	printEp("GET /feed", &feedHist)
	printEp("GET /posts/:id", &postHist)
	printEp("POST /like", &likeHist)
	printEp("POST /posts", &createHist)

	fmt.Println("\n--- HTTP STATUS CODES ---")
	for code := 0; code < 600; code++ {
		c := statusCounts[code].Load()
		if c > 0 {
			fmt.Printf("HTTP %d: %d\n", code, c)
		}
	}

	fmt.Println("\n--- OFFICIAL CHALLENGE THRESHOLDS ---")
	fmt.Printf("Criterion 1: p95 < 500 ms    -> %v [%s]\n", p95, statusTag(p95 < 500*time.Millisecond))
	fmt.Printf("Criterion 2: p99 < 1000 ms   -> %v [%s]\n", p99, statusTag(p99 < 1000*time.Millisecond))
	fmt.Printf("Criterion 3: Errors < 1.0%%   -> %.3f%% [%s]\n", errPct, statusTag(errPct < 1.0))

	if passed {
		fmt.Println("\nVERDICT: PASS! All official challenge criteria met.")
	} else {
		fmt.Println("\nVERDICT: FAIL! One or more official thresholds exceeded.")
	}
	fmt.Println("========================================================================")
}

func statusTag(ok bool) string {
	if ok {
		return "PASS"
	}
	return "FAIL"
}