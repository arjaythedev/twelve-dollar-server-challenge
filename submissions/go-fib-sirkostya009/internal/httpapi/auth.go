package httpapi

import (
	"context"
	"net/http"
	"strings"

	"feed/domain"
)

const bearer = "Bearer "

// authenticate checks the request's Authorization header, if it has one, and
// records the outcome in ctx for requireUser. A request without the header is
// left as is: requireUser reads that as ErrMissingToken, and the public reads
// pay nothing.
func (s *Server) authenticate(r *http.Request) *http.Request {
	h, ok := r.Header["Authorization"]
	if !ok || len(h) == 0 {
		return r
	}
	token, isBearer := strings.CutPrefix(h[0], bearer)
	if !isBearer {
		return r.WithContext(domain.WithAuth(r.Context(), domain.Principal{}, domain.ErrMissingToken))
	}
	p, err := s.tokens.Verify(token)
	return r.WithContext(domain.WithAuth(r.Context(), p, err))
}

// requireUser guards the write routes: past it, ctx carries a principal.
func requireUser(ctx context.Context, w http.ResponseWriter) bool {
	if _, err := domain.PrincipalFrom(ctx); err != nil {
		unauthorized(w, err)
		return false
	}
	return true
}
