// Package httpapi is the http layer: rr routes over the service layer.
package httpapi

import (
	"fmt"
	"log/slog"
	"net/http"
	"time"

	"feed/domain"
	"feed/internal/auth"
	"feed/internal/repository"
	"feed/internal/service"
)

// Server is the generated router behind authentication and panic recovery.
type Server struct {
	Api

	tokens *auth.Verifier
}

// New wires the http layer. It does no I/O.
func New(db repository.Pinger, services service.Container, jwtSecret string) *Server {
	return &Server{
		DB: db, Started: time.Now(),
		FeedApi:  FeedApi{Posts: services.Posts},
		PostsApi: PostsApi{Posts: services.Posts},
		tokens:   auth.NewVerifier([]byte(jwtSecret)),
	}
}

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	defer func() {
		if rec := recover(); rec != nil {
			if rec == http.ErrAbortHandler {
				panic(rec)
			}
			slog.Error("panic", slog.String("err", fmt.Sprint(rec)), slog.String("path", r.URL.Path))
			writeError(w, http.StatusInternalServerError, domain.ErrInternal)
		}
	}()
	s.Api.ServeHTTP(w, s.authenticate(r))
}
