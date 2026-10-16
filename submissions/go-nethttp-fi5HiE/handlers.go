package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
)

const maxBodyChars = 500

type Server struct {
	store   *Store
	auth    *Verifier
	log     *slog.Logger
	started time.Time
}

func newServer(st *Store, v *Verifier, log *slog.Logger) *Server {
	return &Server{store: st, auth: v, log: log, started: time.Now()}
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", s.health)
	mux.HandleFunc("GET /feed", s.feed)
	mux.HandleFunc("GET /posts/{id}", s.getPost)
	mux.HandleFunc("POST /posts", s.createPost)
	mux.HandleFunc("POST /posts/{id}/like", s.like)
	mux.HandleFunc("/", func(w http.ResponseWriter, _ *http.Request) {
		writeError(w, http.StatusNotFound, "not found")
	})
	return mux
}

func (s *Server) health(w http.ResponseWriter, _ *http.Request) {
	if err := s.store.Ping(); err != nil {
		writeJSON(w, http.StatusServiceUnavailable, struct {
			Status string `json:"status"`
			DB     string `json:"db"`
			Error  string `json:"error"`
		}{"degraded", "unreachable", err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, struct {
		Status  string `json:"status"`
		DB      string `json:"db"`
		UptimeS int64  `json:"uptime_s"`
	}{"ok", "ok", int64(time.Since(s.started).Seconds())})
}

func (s *Server) feed(w http.ResponseWriter, r *http.Request) {
	body, err := s.store.FeedJSON()
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Content-Length", strconv.Itoa(len(body)))
	_, _ = io.WriteString(w, body)
}

func (s *Server) getPost(w http.ResponseWriter, r *http.Request) {
	id, ok := parseID(r.PathValue("id"))
	if !ok {
		writeError(w, http.StatusBadRequest, "invalid post id")
		return
	}
	p, err := s.store.Post(id)
	if errors.Is(err, errPostNotFound) {
		writeError(w, http.StatusNotFound, "post not found")
		return
	}
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	writePost(w, http.StatusOK, p)
}

func (s *Server) createPost(w http.ResponseWriter, r *http.Request) {
	user, ok := s.authenticate(w, r)
	if !ok {
		return
	}
	body, msg := decodePostBody(r)
	if msg != "" {
		writeError(w, http.StatusBadRequest, msg)
		return
	}
	p, err := s.store.CreatePost(user.ID, user.Username, body)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	writePost(w, http.StatusCreated, p)
}

func (s *Server) like(w http.ResponseWriter, r *http.Request) {
	user, ok := s.authenticate(w, r)
	if !ok {
		return
	}
	id, ok := parseID(r.PathValue("id"))
	if !ok {
		writeError(w, http.StatusBadRequest, "invalid post id")
		return
	}
	already, err := s.store.Like(user.ID, id)
	if errors.Is(err, errPostNotFound) {
		writeError(w, http.StatusNotFound, "post not found")
		return
	}
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	status := http.StatusCreated
	if already {
		status = http.StatusOK
	}
	writeJSON(w, status, struct {
		Liked        bool  `json:"liked"`
		AlreadyLiked bool  `json:"already_liked"`
		PostID       int64 `json:"post_id"`
	}{true, already, id})
}

func (s *Server) authenticate(w http.ResponseWriter, r *http.Request) (User, bool) {
	token, ok := strings.CutPrefix(r.Header.Get("Authorization"), "Bearer ")
	if !ok {
		writeError(w, http.StatusUnauthorized, "missing bearer token")
		return User{}, false
	}
	user, err := s.auth.Verify(token)
	switch {
	case errors.Is(err, errInvalidPayload):
		writeError(w, http.StatusUnauthorized, "invalid token payload")
		return User{}, false
	case err != nil:
		writeError(w, http.StatusUnauthorized, "invalid or expired token")
		return User{}, false
	}
	return user, true
}

func decodePostBody(r *http.Request) (body, errMsg string) {
	raw, err := io.ReadAll(http.MaxBytesReader(nil, r.Body, 64<<10))
	if err != nil || !json.Valid(raw) {
		return "", "malformed JSON body"
	}
	var req struct {
		Body *string `json:"body"`
	}
	if err := json.Unmarshal(raw, &req); err != nil || req.Body == nil {
		return "", "body is required"
	}
	body = strings.TrimSpace(*req.Body)
	switch {
	case body == "":
		return "", "body is required"
	case utf8.RuneCountInString(body) > maxBodyChars:
		return "", "body must be at most 500 characters"
	}
	return body, ""
}

func parseID(s string) (int64, bool) {
	if s == "" || strings.TrimLeft(s, "0123456789") != "" {
		return 0, false
	}
	n, err := strconv.ParseInt(s, 10, 64)
	return n, err == nil && n > 0
}

func (s *Server) internalError(w http.ResponseWriter, r *http.Request, err error) {
	if !errors.Is(err, context.Canceled) {
		s.log.Error("request failed", "method", r.Method, "path", r.URL.Path, "err", err)
	}
	writeError(w, http.StatusInternalServerError, "internal server error")
}

func writePost(w http.ResponseWriter, status int, p Post) {
	writeJSON(w, status, struct {
		Post Post `json:"post"`
	}{p})
}

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, struct {
		Error string `json:"error"`
	}{msg})
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
