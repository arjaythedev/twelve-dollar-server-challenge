// Package httpapi is the http layer: rr routes over the service layer.
package httpapi

import (
	"fmt"
	"log/slog"
	"net/http"
	"time"

	fibhttp "github.com/lesismal/fib/http"

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
		DB:       db,
		Started:  time.Now(),
		FeedApi:  FeedApi{Posts: services.Posts},
		PostsApi: PostsApi{Posts: services.Posts},
		tokens:   auth.NewVerifier([]byte(jwtSecret)),
	}
}

func (s *Server) ServeHTTP(c *fibhttp.Context) {
	defer func() {
		if rec := recover(); rec != nil {
			slog.Error("panic", slog.String("err", fmt.Sprint(rec)), slog.String("path", c.Request.URL.Path))
			writeError(c, http.StatusInternalServerError, domain.ErrInternal)
		}
	}()
	s.Api.ServeHTTP(c, s.authenticate(c.Request))
}
