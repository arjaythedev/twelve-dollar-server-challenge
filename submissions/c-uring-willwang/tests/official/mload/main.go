// mload: the challenge's bench/load.js user model in Go, light enough for ~250k users per box.
//
// One user = one goroutine with its own keep-alive TCP connection, doing exactly what load.js does:
//
//	GET /feed -> think 3-7 s -> GET /posts/:id (one from the feed) -> think 3-8 s
//	-> like it (15%) -> write a post (2%) -> idle 5-15 s -> repeat
//
// Users ramp in linearly over -ramp (and sleep 0-5 s before their first request, like load.js), then
// the hold runs for -hold. Every request in the run counts toward the result, as in k6's thresholds.
//
//	run:   mload -target 1.2.3.4,1.2.3.5 -ports 80,81 -users 250000 -offset 0 -gzip -think-scale 2.5 -tokens tokens.json -out lg1.json
//	merge: mload -merge -out total.json lg1.json lg2.json ...
//
// Pass: p95 < 500 ms, p99 < 1000 ms, failed requests < 1% (status >= 400 or no response).
package main

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"math"
	"math/rand/v2"
	"net"
	"os"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

var (
	target   = flag.String("target", "", "server IPs, comma list (users round-robin over IPs, then ports)")
	portsArg = flag.String("ports", "80", "server ports, comma list or lo-hi (users round-robin over them)")
	users    = flag.Int("users", 1000, "users on this load generator")
	offset   = flag.Int("offset", 0, "first user number (so load generators use different tokens)")
	ramp     = flag.Duration("ramp", 60*time.Second, "ramp-up")
	hold     = flag.Duration("hold", 5*time.Minute, "hold after ramp")
	startAt  = flag.Int64("start", 0, "unix ms to start (sync several load generators); 0 = now")
	tokensF  = flag.String("tokens", "tokens.json", "seed/tokens.json")
	outF     = flag.String("out", "", "write the result JSON here")
	merge    = flag.Bool("merge", false, "merge result files given as arguments")
	timeout  = flag.Duration("timeout", 60*time.Second, "per-request timeout (k6 default 60 s)")
	gzipOK   = flag.Bool("gzip", false, "send Accept-Encoding: gzip like a browser (k6 doesn't)")
	thinkX   = flag.Float64("think-scale", 1, "multiply every think and idle time (2.5: a page every ~24 s, 1M users ~42k req/s)")
)

// ---- latency histogram: 1% log buckets from 1 µs ----

const nb = 2400 // 1.01^2400 µs ≫ 60 s

func bucket(d time.Duration) int {
	us := float64(d.Microseconds())
	if us < 1 {
		return 0
	}
	b := int(math.Log(us) / math.Log(1.01))
	if b >= nb {
		b = nb - 1
	}
	return b
}

func bucketMs(b int) float64 { return math.Pow(1.01, float64(b)+0.5) / 1000 }

type hist [nb]atomic.Int64

func pct(h []int64, p float64) float64 {
	var n int64
	for _, c := range h {
		n += c
	}
	if n == 0 {
		return 0
	}
	want := int64(math.Ceil(p * float64(n)))
	var acc int64
	for i, c := range h {
		acc += c
		if acc >= want {
			return bucketMs(i)
		}
	}
	return bucketMs(nb - 1)
}

// ---- stats ----

var endpoints = []string{"GET /feed", "GET /posts/:id", "POST /posts/:id/like", "POST /posts"}

type epStats struct {
	ok, fail atomic.Int64
	h        hist
}

var (
	stats     [4]epStats
	all       hist
	connHist  hist         // TCP connect times (not part of request latency, as in k6)
	active    atomic.Int64 // users started
	conns     atomic.Int64 // open connections
	dials     atomic.Int64
	errMu     sync.Mutex
	errKinds  = map[string]int64{}
	timeline  []tick
	tlMu      sync.Mutex
	startTime time.Time
)

type tick struct {
	T     int     `json:"t"`
	Users int64   `json:"users"`
	Conns int64   `json:"conns"`
	Reqs  int64   `json:"reqs"`
	Fails int64   `json:"fails"`
	P95ms float64 `json:"p95_ms"`
	P99ms float64 `json:"p99_ms"`
	RSSMB int     `json:"rss_mb"`
}

func noteErr(kind string) {
	errMu.Lock()
	errKinds[kind]++
	errMu.Unlock()
}

// ---- tokens ----

type token struct {
	Username string `json:"username"`
	Token    string `json:"token"`
}

// ---- one user ----

type user struct {
	n       int
	host    string
	port    string
	auth    string
	name    string
	c       net.Conn
	r       *bufio.Reader
	w       *bufio.Writer
	rng     *rand.Rand
	body    []byte
	iter    int
	gzipped bool // the last response body is gzip-compressed

	connecting time.Duration // time spent dialing during the current request
}

func (u *user) between(lo, hi float64) time.Duration {
	return time.Duration((lo + u.rng.Float64()*(hi-lo)) * *thinkX * float64(time.Second))
}

var errStatus = errors.New("status")

// do sends one request and reads the whole response into u.body. Returns status (0 = no response).
func (u *user) do(ep int, method, path string, auth bool, payload []byte) int {
	t0 := time.Now()
	u.connecting = 0
	status, err := u.roundTrip(method, path, auth, payload)
	if err != nil && u.c != nil {
		// a stale keep-alive socket: retry once on a fresh connection, like an HTTP client would
		if status == 0 && method == "GET" && !isTimeout(err) {
			u.close()
			status, err = u.roundTrip(method, path, auth, payload)
		}
	}
	// Like k6's http_req_duration: sending + waiting + receiving, not connecting
	// (a failed connect still counts as a failed request).
	d := time.Since(t0) - u.connecting
	all[bucket(d)].Add(1)
	stats[ep].h[bucket(d)].Add(1)
	if err != nil || status >= 400 || status == 0 {
		stats[ep].fail.Add(1)
		switch {
		case err != nil:
			noteErr(errKind(err))
			u.close()
		default:
			noteErr("http " + strconv.Itoa(status))
		}
		return status
	}
	stats[ep].ok.Add(1)
	return status
}

func isTimeout(err error) bool {
	var ne net.Error
	return errors.As(err, &ne) && ne.Timeout()
}

func errKind(err error) string {
	if isTimeout(err) {
		return "timeout"
	}
	s := err.Error()
	if i := strings.LastIndex(s, ": "); i >= 0 {
		s = s[i+2:]
	}
	return s
}

func (u *user) close() {
	if u.c != nil {
		u.c.Close()
		u.c = nil
		conns.Add(-1)
	}
}

func (u *user) roundTrip(method, path string, auth bool, payload []byte) (int, error) {
	if u.c == nil {
		d := net.Dialer{Timeout: *timeout}
		tc := time.Now()
		c, err := d.Dial("tcp", net.JoinHostPort(u.host, u.port))
		ct := time.Since(tc)
		u.connecting += ct
		connHist[bucket(ct)].Add(1)
		dials.Add(1)
		if err != nil {
			return 0, err
		}
		u.c = c
		conns.Add(1)
		if u.r == nil {
			u.r = bufio.NewReaderSize(c, 4096)
			u.w = bufio.NewWriterSize(c, 1024)
		} else {
			u.r.Reset(c)
			u.w.Reset(c)
		}
	}
	u.c.SetDeadline(time.Now().Add(*timeout))
	w := u.w
	w.WriteString(method)
	w.WriteByte(' ')
	w.WriteString(path)
	w.WriteString(" HTTP/1.1\r\nHost: ")
	w.WriteString(u.host)
	w.WriteString("\r\nUser-Agent: mload\r\n")
	if *gzipOK {
		w.WriteString("Accept-Encoding: gzip\r\n")
	}
	if auth {
		w.WriteString(u.auth)
	}
	if payload != nil {
		w.WriteString("Content-Type: application/json\r\nContent-Length: ")
		w.WriteString(strconv.Itoa(len(payload)))
		w.WriteString("\r\n\r\n")
		w.Write(payload)
	} else if method == "POST" {
		w.WriteString("Content-Length: 0\r\n\r\n")
	} else {
		w.WriteString("\r\n")
	}
	if err := w.Flush(); err != nil {
		return 0, err
	}
	return u.readResponse()
}

func (u *user) readResponse() (int, error) {
	line, err := u.r.ReadSlice('\n')
	if err != nil {
		return 0, err
	}
	if len(line) < 12 || !bytes.HasPrefix(line, []byte("HTTP/1.")) {
		return 0, fmt.Errorf("bad status line")
	}
	status, err := strconv.Atoi(string(line[9:12]))
	if err != nil {
		return 0, fmt.Errorf("bad status line")
	}
	clen, chunked, closeConn := -1, false, false
	u.gzipped = false
	for {
		h, err := u.r.ReadSlice('\n')
		if err != nil {
			return 0, err
		}
		if len(h) <= 2 {
			break
		}
		k, v, ok := bytes.Cut(h, []byte(":"))
		if !ok {
			continue
		}
		v = bytes.TrimSpace(v)
		switch {
		case bytes.EqualFold(k, []byte("content-length")):
			clen, _ = strconv.Atoi(string(v))
		case bytes.EqualFold(k, []byte("transfer-encoding")):
			chunked = bytes.Contains(bytes.ToLower(v), []byte("chunked"))
		case bytes.EqualFold(k, []byte("connection")):
			closeConn = bytes.EqualFold(v, []byte("close"))
		case bytes.EqualFold(k, []byte("content-encoding")):
			u.gzipped = bytes.EqualFold(v, []byte("gzip"))
		}
	}
	u.body = u.body[:0]
	switch {
	case chunked:
		for {
			l, err := u.r.ReadSlice('\n')
			if err != nil {
				return 0, err
			}
			l = bytes.TrimSpace(l)
			if i := bytes.IndexByte(l, ';'); i >= 0 {
				l = l[:i]
			}
			n, err := strconv.ParseInt(string(l), 16, 64)
			if err != nil {
				return 0, fmt.Errorf("bad chunk")
			}
			if n == 0 {
				for { // trailers
					t, err := u.r.ReadSlice('\n')
					if err != nil {
						return 0, err
					}
					if len(t) <= 2 {
						break
					}
				}
				break
			}
			if err := u.readN(int(n)); err != nil {
				return 0, err
			}
			if _, err := u.r.Discard(2); err != nil {
				return 0, err
			}
		}
	case clen >= 0:
		if err := u.readN(clen); err != nil {
			return 0, err
		}
	default:
		b, err := io.ReadAll(u.r)
		if err != nil {
			return 0, err
		}
		u.body = append(u.body, b...)
		closeConn = true
	}
	if closeConn {
		u.close()
	}
	return status, nil
}

func (u *user) readN(n int) error {
	start := len(u.body)
	if cap(u.body)-start < n {
		nb := make([]byte, start, start+n)
		copy(nb, u.body)
		u.body = nb
	}
	u.body = u.body[:start+n]
	_, err := io.ReadFull(u.r, u.body[start:])
	return err
}

var idKey = []byte(`{"id":`)

var gzReaders sync.Pool

// gunzip decompresses a feed body (pooled readers: a gzip reader is 32+ KB, too big per user).
func gunzip(b []byte) []byte {
	zr, _ := gzReaders.Get().(*gzip.Reader)
	var err error
	if zr == nil {
		zr, err = gzip.NewReader(bytes.NewReader(b))
	} else {
		err = zr.Reset(bytes.NewReader(b))
	}
	if err != nil {
		return nil
	}
	out, _ := io.ReadAll(zr)
	gzReaders.Put(zr)
	return out
}

// pickPost returns a random post id from a feed body, or 0.
func (u *user) pickPost() int64 {
	var ids [32]int64
	n := 0
	b := u.body
	if u.gzipped {
		b = gunzip(b)
	}
	for n < len(ids) {
		i := bytes.Index(b, idKey)
		if i < 0 {
			break
		}
		b = b[i+len(idKey):]
		j := 0
		for j < len(b) && b[j] >= '0' && b[j] <= '9' {
			j++
		}
		if id, err := strconv.ParseInt(string(b[:j]), 10, 64); err == nil {
			ids[n] = id
			n++
		}
	}
	if n == 0 {
		return 0
	}
	return ids[u.rng.IntN(n)]
}

func (u *user) run(stop <-chan struct{}) {
	sleep := func(d time.Duration) bool {
		t := time.NewTimer(d)
		select {
		case <-t.C:
			return true
		case <-stop:
			t.Stop()
			return false
		}
	}
	if !sleep(u.between(0, 5)) {
		return
	}
	for {
		st := u.do(0, "GET", "/feed", false, nil)
		if st != 200 {
			if !sleep(u.between(3, 7)) {
				return
			}
			continue
		}
		id := u.pickPost()
		if !sleep(u.between(3, 7)) {
			return
		}
		u.do(1, "GET", "/posts/"+strconv.FormatInt(id, 10), false, nil)
		if !sleep(u.between(3, 8)) {
			return
		}
		if u.rng.Float64() < 0.15 {
			u.do(2, "POST", "/posts/"+strconv.FormatInt(id, 10)+"/like", true, nil)
		}
		if u.rng.Float64() < 0.02 {
			body := fmt.Sprintf(`{"body":"%s says hi at %s (VU %d, iter %d)"}`, u.name, time.Now().UTC().Format("2006-01-02T15:04:05.000Z"), u.n+1, u.iter)
			u.do(3, "POST", "/posts", true, []byte(body))
		}
		u.iter++
		if !sleep(u.between(5, 15)) {
			return
		}
	}
}

// ---- result ----

type epResult struct {
	Name  string  `json:"name"`
	OK    int64   `json:"ok"`
	Fail  int64   `json:"fail"`
	P95ms float64 `json:"p95_ms"`
	P99ms float64 `json:"p99_ms"`
	Hist  []int64 `json:"hist"`
}

type result struct {
	Sources      []string         `json:"sources,omitempty"`
	Users        int              `json:"users"`
	DurationS    float64          `json:"duration_s"`
	Reqs         int64            `json:"reqs"`
	Fails        int64            `json:"fails"`
	ReqPerS      float64          `json:"req_per_s"`
	ErrPct       float64          `json:"err_pct"`
	P50ms        float64          `json:"p50_ms"`
	P95ms        float64          `json:"p95_ms"`
	P99ms        float64          `json:"p99_ms"`
	MaxMs        float64          `json:"max_ms"`
	Pass         bool             `json:"pass"`
	Endpoints    []epResult       `json:"endpoints"`
	Errors       map[string]int64 `json:"errors"`
	Hist         []int64          `json:"hist"`
	Dials        int64            `json:"dials"`
	ConnectP95ms float64          `json:"connect_p95_ms"`
	ConnectP99ms float64          `json:"connect_p99_ms"`
	ConnectHist  []int64          `json:"connect_hist"`
	Timeline     []tick           `json:"timeline,omitempty"`
}

func snapshot(h *hist) []int64 {
	out := make([]int64, nb)
	for i := range h {
		out[i] = h[i].Load()
	}
	return out
}

func finish(r *result) {
	r.Reqs, r.Fails = 0, 0
	for i := range r.Endpoints {
		e := &r.Endpoints[i]
		r.Reqs += e.OK + e.Fail
		r.Fails += e.Fail
		e.P95ms, e.P99ms = pct(e.Hist, .95), pct(e.Hist, .99)
	}
	if r.DurationS > 0 {
		r.ReqPerS = float64(r.Reqs) / r.DurationS
	}
	if r.Reqs > 0 {
		r.ErrPct = 100 * float64(r.Fails) / float64(r.Reqs)
	}
	r.P50ms, r.P95ms, r.P99ms = pct(r.Hist, .5), pct(r.Hist, .95), pct(r.Hist, .99)
	for i := nb - 1; i >= 0; i-- {
		if r.Hist[i] > 0 {
			r.MaxMs = bucketMs(i)
			break
		}
	}
	if r.ConnectHist != nil {
		r.ConnectP95ms, r.ConnectP99ms = pct(r.ConnectHist, .95), pct(r.ConnectHist, .99)
	}
	r.Pass = r.Reqs > 0 && r.P95ms < 500 && r.P99ms < 1000 && r.ErrPct < 1
}

func rssMB() int {
	b, _ := os.ReadFile("/proc/self/statm")
	f := strings.Fields(string(b))
	if len(f) < 2 {
		return 0
	}
	p, _ := strconv.Atoi(f[1])
	return p * os.Getpagesize() >> 20
}

func parsePorts(s string) []string {
	var out []string
	for _, part := range strings.Split(s, ",") {
		lo, hi, isRange := strings.Cut(part, "-")
		if !isRange {
			out = append(out, part)
			continue
		}
		a, _ := strconv.Atoi(lo)
		b, _ := strconv.Atoi(hi)
		for p := a; p <= b; p++ {
			out = append(out, strconv.Itoa(p))
		}
	}
	return out
}

func main() {
	flag.Parse()
	if *merge {
		doMerge(flag.Args())
		return
	}
	var toks []token
	b, err := os.ReadFile(*tokensF)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	if err := json.Unmarshal(b, &toks); err != nil || len(toks) == 0 {
		fmt.Fprintln(os.Stderr, "tokens:", err)
		os.Exit(2)
	}
	ports := parsePorts(*portsArg)
	hosts := strings.Split(*target, ",")

	if *startAt > 0 {
		time.Sleep(time.Until(time.UnixMilli(*startAt)))
	}
	startTime = time.Now()
	stop := make(chan struct{})
	var wg sync.WaitGroup

	// progress: every 5 s, the requests and latency of that window
	go func() {
		var prev [nb]int64
		var prevReqs, prevFails int64
		for range time.Tick(5 * time.Second) {
			cur := snapshot(&all)
			win := make([]int64, nb)
			var reqs int64
			for i := range cur {
				win[i] = cur[i] - prev[i]
				prev[i] = cur[i]
				reqs += win[i]
			}
			var fails int64
			for i := range stats {
				fails += stats[i].fail.Load()
			}
			t := tick{T: int(time.Since(startTime).Seconds()), Users: active.Load(), Conns: conns.Load(),
				Reqs: reqs, Fails: fails - prevFails, P95ms: pct(win, .95), P99ms: pct(win, .99), RSSMB: rssMB()}
			_ = prevReqs
			prevFails = fails
			tlMu.Lock()
			timeline = append(timeline, t)
			tlMu.Unlock()
			fmt.Printf("t=%4ds users=%d conns=%d req/s=%.0f fails=%d p95=%.1fms p99=%.1fms rss=%dMB\n",
				t.T, t.Users, t.Conns, float64(t.Reqs)/5, t.Fails, t.P95ms, t.P99ms, t.RSSMB)
		}
	}()

	// ramp: start user i at i/users * ramp
	for i := 0; i < *users; i++ {
		at := startTime.Add(time.Duration(float64(*ramp) * float64(i) / float64(*users)))
		if d := time.Until(at); d > 0 {
			time.Sleep(d)
		}
		n := *offset + i
		tk := toks[n%len(toks)]
		u := &user{n: n, host: hosts[n%len(hosts)], port: ports[(n/len(hosts))%len(ports)], name: tk.Username,
			auth: "Authorization: Bearer " + tk.Token + "\r\n",
			rng:  rand.New(rand.NewPCG(uint64(n), uint64(time.Now().UnixNano())))}
		active.Add(1)
		wg.Add(1)
		go func() { defer wg.Done(); u.run(stop) }()
	}
	time.Sleep(time.Until(startTime.Add(*ramp + *hold)))
	dur := time.Since(startTime).Seconds()

	// snapshot before stopping, so the shutdown doesn't add errors
	r := result{Users: *users, DurationS: dur, Hist: snapshot(&all), Dials: dials.Load(), ConnectHist: snapshot(&connHist)}
	for i := range stats {
		r.Endpoints = append(r.Endpoints, epResult{Name: endpoints[i], OK: stats[i].ok.Load(), Fail: stats[i].fail.Load(), Hist: snapshot(&stats[i].h)})
	}
	// Copies, not the live map/slice: user goroutines keep recording until stop is closed below, and handing
	// the live map to json/fmt crashed mload ("concurrent map iteration and map write") on runs with many errors.
	errMu.Lock()
	r.Errors = make(map[string]int64, len(errKinds))
	for k, v := range errKinds {
		r.Errors[k] = v
	}
	errMu.Unlock()
	tlMu.Lock()
	r.Timeline = append([]tick(nil), timeline...)
	tlMu.Unlock()
	finish(&r)
	close(stop)
	write(&r)
	fmt.Printf("RESULT users=%d req/s=%.0f p95=%.1fms p99=%.1fms err=%.3f%% pass=%v errors=%v\n",
		r.Users, r.ReqPerS, r.P95ms, r.P99ms, r.ErrPct, r.Pass, r.Errors)
	_ = runtime.NumGoroutine
}

func write(r *result) {
	if *outF == "" {
		return
	}
	b, _ := json.Marshal(r)
	os.WriteFile(*outF, b, 0o644)
}

func doMerge(files []string) {
	var m result
	m.Hist = make([]int64, nb)
	m.Errors = map[string]int64{}
	for _, f := range files {
		b, err := os.ReadFile(f)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(2)
		}
		var r result
		if err := json.Unmarshal(b, &r); err != nil {
			fmt.Fprintln(os.Stderr, f, err)
			os.Exit(2)
		}
		m.Sources = append(m.Sources, f)
		m.Users += r.Users
		m.Dials += r.Dials
		if m.ConnectHist == nil {
			m.ConnectHist = make([]int64, nb)
		}
		for i := range r.ConnectHist {
			m.ConnectHist[i] += r.ConnectHist[i]
		}
		if r.DurationS > m.DurationS {
			m.DurationS = r.DurationS
		}
		for i := range r.Hist {
			m.Hist[i] += r.Hist[i]
		}
		if m.Endpoints == nil {
			for _, e := range r.Endpoints {
				m.Endpoints = append(m.Endpoints, epResult{Name: e.Name, Hist: make([]int64, nb)})
			}
		}
		for i, e := range r.Endpoints {
			m.Endpoints[i].OK += e.OK
			m.Endpoints[i].Fail += e.Fail
			for j := range e.Hist {
				m.Endpoints[i].Hist[j] += e.Hist[j]
			}
		}
		for k, v := range r.Errors {
			m.Errors[k] += v
		}
	}
	finish(&m)
	write(&m)
	fmt.Printf("RESULT users=%d req/s=%.0f p50=%.1fms p95=%.1fms p99=%.1fms max=%.0fms err=%.3f%% pass=%v errors=%v\n",
		m.Users, m.ReqPerS, m.P50ms, m.P95ms, m.P99ms, m.MaxMs, m.ErrPct, m.Pass, m.Errors)
	fmt.Printf("  %-22s dials=%d p95=%.1fms p99=%.1fms (not in request latency, as in k6)\n", "TCP connect", m.Dials, m.ConnectP95ms, m.ConnectP99ms)
	for _, e := range m.Endpoints {
		fmt.Printf("  %-22s ok=%d fail=%d p95=%.1fms p99=%.1fms\n", e.Name, e.OK, e.Fail, e.P95ms, e.P99ms)
	}
}
