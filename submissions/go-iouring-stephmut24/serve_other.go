//go:build !linux

package main

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"runtime"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
)

// net/http path for local Darwin/Windows testing. The Linux build uses io_uring/epoll.
func serve(host, port, secret string) error {
	runtime.GOMAXPROCS(1)
	s := &httpServer{jwt: newVerifier(secret), start: time.Now()}
	mux := http.NewServeMux()
	mux.HandleFunc("/", s.handle)
	addr := net.JoinHostPort(host, port)
	log.Printf("listening on %s (net/http)", addr)
	return http.ListenAndServe(addr, mux)
}

type httpServer struct {
	jwt   *verifier
	start time.Time
}

func (s *httpServer) handle(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Path
	switch {
	case r.Method == http.MethodGet && path == "/health":
		s.health(w)
	case r.Method == http.MethodGet && path == "/feed":
		s.feed(w)
	case r.Method == http.MethodGet && strings.HasPrefix(path, "/posts/") && !strings.Contains(path[7:], "/"):
		s.getPost(w, path[7:])
	case r.Method == http.MethodPost && path == "/posts":
		s.create(w, r)
	case r.Method == http.MethodPost && strings.HasPrefix(path, "/posts/") && strings.HasSuffix(path, "/like") &&
		!strings.Contains(path[7:len(path)-5], "/"):
		s.like(w, r, path[7:len(path)-5])
	default:
		writeJSON(w, 404, errNotFound)
	}
}

func (s *httpServer) health(w http.ResponseWriter) {
	if msg, ok := dbPing(); !ok {
		b, _ := json.Marshal(map[string]string{"status": "degraded", "db": "unreachable", "error": msg})
		writeJSON(w, 503, b)
		return
	}
	body := fmt.Appendf(nil, `{"status":"ok","db":"ok","uptime_s":%d}`, int(time.Since(s.start)/time.Second))
	writeJSON(w, 200, body)
}

func (s *httpServer) feed(w http.ResponseWriter) {
	buf := make([]byte, feedRoom)
	n := dbFeed(buf)
	if n < 0 {
		writeJSON(w, 500, errInternal)
		return
	}
	writeJSON(w, 200, buf[:n])
}

func (s *httpServer) getPost(w http.ResponseWriter, raw string) {
	id, valid, tooBig := parseID([]byte(raw))
	switch {
	case !valid:
		writeJSON(w, 400, errInvalidID)
		return
	case tooBig:
		writeJSON(w, 404, errPostNotFound)
		return
	}
	buf := make([]byte, postRoom)
	n := dbPost(id, buf)
	switch {
	case n < 0:
		writeJSON(w, 500, errInternal)
	case n == 0:
		writeJSON(w, 404, errPostNotFound)
	default:
		writeJSON(w, 200, buf[:n])
	}
}

func (s *httpServer) create(w http.ResponseWriter, r *http.Request) {
	auth := r.Header.Get("Authorization")
	userID, username, authErr := s.jwt.auth([]byte(auth), auth != "")
	if authErr != nil {
		writeJSON(w, 401, authErr)
		return
	}
	raw, err := io.ReadAll(io.LimitReader(r.Body, maxBodyBytes+1))
	if err != nil {
		writeJSON(w, 400, errMalformedJSON)
		return
	}
	var v any
	if json.Unmarshal(raw, &v) != nil {
		writeJSON(w, 400, errMalformedJSON)
		return
	}
	obj, _ := v.(map[string]any)
	body, ok := obj["body"].(string)
	if !ok {
		writeJSON(w, 400, errBodyRequired)
		return
	}
	body = strings.TrimFunc(body, isJSSpace)
	if body == "" {
		writeJSON(w, 400, errBodyRequired)
		return
	}
	if utf8.RuneCountInString(body) > 500 {
		writeJSON(w, 400, errBodyTooLong)
		return
	}
	if !dbBegin() {
		writeJSON(w, 500, errInternal)
		return
	}
	out := make([]byte, postRoom)
	n := dbCreate(userID, body, username, out)
	if n < 0 || !dbCommit() {
		dbRollback()
		writeJSON(w, 500, errInternal)
		return
	}
	writeJSON(w, 201, out[:n])
}

func (s *httpServer) like(w http.ResponseWriter, r *http.Request, rawID string) {
	auth := r.Header.Get("Authorization")
	userID, _, authErr := s.jwt.auth([]byte(auth), auth != "")
	if authErr != nil {
		writeJSON(w, 401, authErr)
		return
	}
	postID, valid, tooBig := parseID([]byte(rawID))
	switch {
	case !valid:
		writeJSON(w, 400, errInvalidID)
		return
	case tooBig:
		writeJSON(w, 404, errPostNotFound)
		return
	}
	if !dbBegin() {
		writeJSON(w, 500, errInternal)
		return
	}
	var status int
	switch dbLike(userID, postID) {
	case 1:
		status = 201
	case 2:
		status = 200
	case 0:
		dbCommit()
		writeJSON(w, 404, errPostNotFound)
		return
	default:
		dbRollback()
		writeJSON(w, 500, errInternal)
		return
	}
	if !dbCommit() {
		dbRollback()
		writeJSON(w, 500, errInternal)
		return
	}
	body := []byte(`{"liked":true,"already_liked":` + strconv.FormatBool(status == 200) + `,"post_id":` + strconv.FormatInt(postID, 10) + `}`)
	writeJSON(w, status, body)
}

func writeJSON(w http.ResponseWriter, status int, body []byte) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Content-Length", strconv.Itoa(len(body)))
	w.WriteHeader(status)
	_, _ = w.Write(body)
}
