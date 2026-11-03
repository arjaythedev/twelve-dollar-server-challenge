// Command loadgen replays the user loop of bench/load.js: one keep-alive connection per
// virtual user, the same think times and the same request mix, scored like the challenge
// (p95 < 500 ms, p99 < 1 s, < 1% failed). Each user is a goroutine with a raw TCP connection
// and no buffer while it sleeps, ~10 KB per user against k6's ~280 KB, so one machine can hold
// the real number of open connections. Latency excludes dialing, like k6's http_req_duration.
//
//	go build -o bin/loadgen ./loadgen
//	bin/loadgen -addr 127.0.0.1:3000 -tokens ../../seed/tokens.json -vus 80000 -ramp 60s -hold 300s
//
// One client IP reaches one server port through ~64k source ports; split larger runs over
// several hosts with -offset so every user gets a distinct token.
package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"math/rand/v2"
	"net"
	"os"
	"slices"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

type token struct {
	Username string `json:"username"`
	Token    string `json:"token"`
}

type recorder struct {
	mu        sync.Mutex
	lat       []time.Duration
	byName    [4]int64
	failed    atomic.Int64
	requests  atomic.Int64
	dials     atomic.Int64
	recording atomic.Bool
	errs      sync.Map
}

func (r *recorder) fail(err error) {
	if !r.recording.Load() || err == nil {
		return
	}
	msg := err.Error()
	if i := strings.LastIndex(msg, ": "); i >= 0 {
		msg = msg[i+2:]
	}
	c, _ := r.errs.LoadOrStore(msg, new(atomic.Int64))
	c.(*atomic.Int64).Add(1)
}

var names = [4]string{"GET /feed", "GET /posts/:id", "POST /posts/:id/like", "POST /posts"}

func (r *recorder) add(kind int, d time.Duration, ok bool) {
	if !r.recording.Load() {
		return
	}
	r.requests.Add(1)
	if !ok {
		r.failed.Add(1)
	}
	r.mu.Lock()
	r.lat = append(r.lat, d)
	r.byName[kind]++
	r.mu.Unlock()
}

var bufPool = sync.Pool{New: func() any { b := make([]byte, 0, 16<<10); return &b }}

type vu struct {
	id    int
	addr  string
	host  string
	auth  string
	user  string
	conn  net.Conn
	speed float64
	rec   *recorder
	stop  <-chan struct{}
}

var errStatus = errors.New("unexpected status")

// roundTrip sends one request on the user's connection and reads the whole response. A
// connection the server closed while idle is redialed once, as an HTTP client does.
func (v *vu) roundTrip(req []byte, body *[]byte) (int, time.Duration, error) {
	for attempt := 0; attempt < 2; attempt++ {
		reused := v.conn != nil
		if v.conn == nil {
			c, err := net.DialTimeout("tcp", v.addr, 10*time.Second)
			if err != nil {
				return 0, 0, err
			}
			v.rec.dials.Add(1)
			v.conn = c
		}
		t0 := time.Now()
		v.conn.SetDeadline(t0.Add(30 * time.Second))
		status, err := v.exchange(req, body)
		d := time.Since(t0)
		if err == nil {
			return status, d, nil
		}
		v.conn.Close()
		v.conn = nil
		if !reused || len(*body) > 0 {
			return 0, d, err
		}
	}
	return 0, 0, errors.New("unreachable")
}

func (v *vu) exchange(req []byte, body *[]byte) (int, error) {
	if _, err := v.conn.Write(req); err != nil {
		return 0, err
	}
	b := (*body)[:0]
	for {
		if len(b) == cap(b) {
			b = slices.Grow(b, len(b))
		}
		n, err := v.conn.Read(b[len(b):cap(b)])
		b = b[:len(b)+n]
		*body = b
		if he := bytes.Index(b, []byte("\r\n\r\n")); he >= 0 {
			head := b[:he]
			cl := -1
			if i := bytes.Index(bytes.ToLower(head), []byte("\r\ncontent-length:")); i >= 0 {
				rest := head[i+17:]
				if j := bytes.IndexByte(rest, '\r'); j >= 0 {
					rest = rest[:j]
				}
				cl, _ = strconv.Atoi(string(bytes.TrimSpace(rest)))
			}
			if cl >= 0 && len(b) >= he+4+cl {
				if len(b) < 12 {
					return 0, errStatus
				}
				status, _ := strconv.Atoi(string(b[9:12]))
				*body = b[he+4 : he+4+cl]
				if bytes.Contains(bytes.ToLower(head), []byte("connection: close")) {
					v.conn.Close()
					v.conn = nil
				}
				return status, nil
			}
		}
		if err != nil {
			if err == io.EOF {
				return 0, io.ErrUnexpectedEOF
			}
			return 0, err
		}
	}
}

func (v *vu) sleep(lo, hi float64) bool {
	t := time.NewTimer(time.Duration((lo + rand.Float64()*(hi-lo)) / v.speed * float64(time.Second)))
	select {
	case <-v.stop:
		t.Stop()
		return false
	case <-t.C:
		return true
	}
}

func (v *vu) do(kind int, req []byte, want1, want2 int) []byte {
	bp := bufPool.Get().(*[]byte)
	status, d, err := v.roundTrip(req, bp)
	ok := err == nil && (status == want1 || status == want2)
	if err == nil && !ok {
		err = fmt.Errorf("status %d", status)
	}
	v.rec.fail(err)
	v.rec.add(kind, d, ok)
	var out []byte
	if ok && kind == 0 {
		out = feedIDs(*bp)
	}
	*bp = (*bp)[:0]
	bufPool.Put(bp)
	return out
}

// feedIDs returns the post ids of a /feed body as a compact list of decimal strings.
func feedIDs(b []byte) []byte {
	var ids []byte
	key := []byte(`{"id":`)
	for {
		i := bytes.Index(b, key)
		if i < 0 {
			return ids
		}
		b = b[i+len(key):]
		j := 0
		for j < len(b) && b[j] >= '0' && b[j] <= '9' {
			j++
		}
		ids = append(append(ids, b[:j]...), ',')
	}
}

func (v *vu) run(wg *sync.WaitGroup) {
	defer wg.Done()
	defer func() {
		if v.conn != nil {
			v.conn.Close()
		}
	}()
	getFeed := []byte("GET /feed HTTP/1.1\r\nHost: " + v.host + "\r\nUser-Agent: loadgen\r\n\r\n")
	if !v.sleep(0, 5) {
		return
	}
	for iter := 0; ; iter++ {
		ids := v.do(0, getFeed, 200, 200)
		if len(ids) == 0 {
			if !v.sleep(3, 7) {
				return
			}
			continue
		}
		parts := bytes.Split(bytes.TrimSuffix(ids, []byte(",")), []byte(","))
		id := string(parts[rand.IntN(len(parts))])
		if !v.sleep(3, 7) {
			return
		}
		v.do(1, []byte("GET /posts/"+id+" HTTP/1.1\r\nHost: "+v.host+"\r\nUser-Agent: loadgen\r\n\r\n"), 200, 200)
		if !v.sleep(3, 8) {
			return
		}
		if rand.Float64() < 0.15 {
			v.do(2, []byte("POST /posts/"+id+"/like HTTP/1.1\r\nHost: "+v.host+"\r\nAuthorization: Bearer "+v.auth+
				"\r\nContent-Type: application/json\r\nContent-Length: 0\r\n\r\n"), 200, 201)
		}
		if rand.Float64() < 0.02 {
			body, _ := json.Marshal(map[string]string{"body": fmt.Sprintf("%s says hi at %s (VU %d, iter %d)",
				v.user, time.Now().UTC().Format("2006-01-02T15:04:05.000Z"), v.id, iter)})
			v.do(3, []byte("POST /posts HTTP/1.1\r\nHost: "+v.host+"\r\nAuthorization: Bearer "+v.auth+
				"\r\nContent-Type: application/json\r\nContent-Length: "+strconv.Itoa(len(body))+"\r\n\r\n"+string(body)), 201, 201)
		}
		if !v.sleep(5, 15) {
			return
		}
	}
}

func main() {
	addr := flag.String("addr", "127.0.0.1:3000", "server host:port")
	tokensPath := flag.String("tokens", "seed/tokens.json", "tokens.json from seed/make-seed.sh")
	vus := flag.Int("vus", 1000, "virtual users on this host")
	offset := flag.Int("offset", 0, "first user number (for runs split over several hosts)")
	speed := flag.Float64("speed", 1, "divide every think time by this (1 = the real load.js pacing)")
	ramp := flag.Duration("ramp", 60*time.Second, "ramp-up time")
	hold := flag.Duration("hold", 120*time.Second, "measured time at full load")
	flag.Parse()

	raw, err := os.ReadFile(*tokensPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	var tokens []token
	if err := json.Unmarshal(raw, &tokens); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	host, _, _ := net.SplitHostPort(*addr)

	rec := &recorder{lat: make([]time.Duration, 0, 1<<22)}
	stop := make(chan struct{})
	var wg sync.WaitGroup
	start := time.Now()
	for i := 0; i < *vus; i++ {
		if *ramp > 0 {
			if due := start.Add(*ramp * time.Duration(i) / time.Duration(*vus)); time.Until(due) > 0 {
				time.Sleep(time.Until(due))
			}
		}
		n := *offset + i
		t := tokens[n%len(tokens)]
		v := &vu{id: n + 1, addr: *addr, host: host, auth: t.Token, user: t.Username, speed: *speed, rec: rec, stop: stop}
		wg.Add(1)
		go v.run(&wg)
	}
	// Let the last users through their first request before measuring.
	time.Sleep(time.Duration(5 / *speed * float64(time.Second)))
	fmt.Printf("ramped to %d users in %s; measuring %s\n", *vus, time.Since(start).Round(time.Second), *hold)
	rec.recording.Store(true)
	t0 := time.Now()
	dials0 := rec.dials.Load()
	time.Sleep(*hold)
	rec.recording.Store(false)
	elapsed := time.Since(t0)
	dials := rec.dials.Load() - dials0
	close(stop)

	rec.mu.Lock()
	lat := slices.Clone(rec.lat)
	byName := rec.byName
	rec.mu.Unlock()
	slices.Sort(lat)
	pct := func(p float64) time.Duration {
		if len(lat) == 0 {
			return 0
		}
		return lat[min(len(lat)-1, int(p*float64(len(lat))))]
	}
	n, failed := rec.requests.Load(), rec.failed.Load()
	fmt.Printf("requests %d (%.0f/s)  failed %d (%.3f%%)  new connections during hold %d\n",
		n, float64(n)/elapsed.Seconds(), failed, 100*float64(failed)/float64(max(n, 1)), dials)
	fmt.Printf("p50 %s  p95 %s  p99 %s  max %s\n", pct(0.50), pct(0.95), pct(0.99), pct(1))
	for i, name := range names {
		fmt.Printf("  %s %d", name, byName[i])
	}
	fmt.Println()
	rec.errs.Range(func(k, val any) bool {
		fmt.Printf("  error %q x%d\n", k, val.(*atomic.Int64).Load())
		return true
	})
	pass := pct(0.95) < 500*time.Millisecond && pct(0.99) < time.Second && float64(failed) < 0.01*float64(n)
	fmt.Printf("PASS=%v\n", pass)
	if !pass {
		os.Exit(1)
	}
}
