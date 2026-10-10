package httpapi

import (
	"context"
	"errors"
	"log/slog"
	"net/http"

	"github.com/sirkostya009/ggen"

	"feed/domain"
)

var jsonCT = []string{"application/json"}

// handleError is the central onerror: domain sentinels map to statuses,
// anything else is a logged 500.
func handleError(ctx context.Context, w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, domain.ErrPostNotFound):
		writeError(w, http.StatusNotFound, domain.ErrPostNotFound)
	case errors.Is(err, domain.ErrInvalidPostID):
		writeError(w, http.StatusBadRequest, domain.ErrInvalidPostID)
	default:
		// ctx is r.Context(), cancelled when the client goes away; the error
		// still deserves its log line
		slog.ErrorContext(context.WithoutCancel(ctx), "handler", slog.Any("err", err))
		writeError(w, http.StatusInternalServerError, domain.ErrInternal)
	}
}

// badRequest answers a body that did not decode. err is nil when rr found
// the request malformed with no decode error attached.
func badRequest(w http.ResponseWriter, err error) {
	writeError(w, http.StatusBadRequest, bodyError(err))
}

// bodyError sorts a CreatePost decode error into the spec's three messages.
// A non-string "body" fails at its first byte with ErrExpectString. A broken
// string inside it is still malformed JSON.
func bodyError(err error) error {
	var maxRunes *ggen.MaxRunesError
	var parse *ggen.ParseError
	switch {
	case err == nil:
		return domain.ErrMalformed
	case errors.As(err, &maxRunes):
		return domain.ErrBodyTooLong
	case errors.As(err, &parse):
		if len(parse.Path) > 0 && parse.Path[0] == "body" && errors.Is(parse.Err, ggen.ErrExpectString) {
			return domain.ErrBodyRequired
		}
		return domain.ErrMalformed
	default:
		// required, notempty: the JSON was fine, the body was missing or blank
		return domain.ErrBodyRequired
	}
}

func notFound(w http.ResponseWriter) {
	writeError(w, http.StatusNotFound, domain.ErrNotFound)
}

var errMethodNotAllowed = errors.New("method not allowed")

func methodNotAllowed(w http.ResponseWriter) {
	writeError(w, http.StatusMethodNotAllowed, errMethodNotAllowed)
}

func unauthorized(w http.ResponseWriter, err error) {
	w.Header().Set("WWW-Authenticate", "Bearer")
	writeError(w, http.StatusUnauthorized, err)
}

// writeError writes {"error":"<err>"} with status.
func writeError(w http.ResponseWriter, status int, err error) {
	w.Header()["Content-Type"] = jsonCT
	w.WriteHeader(status)
	_ = ggen.WriteTo(w, domain.ErrorBody{Error: err.Error()})
}
