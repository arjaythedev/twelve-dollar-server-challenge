package domain

import "errors"

// Sentinels the http layer maps to statuses and messages. Their text is the
// message the spec answers with, so they need no second table.
var (
	ErrNotFound      = errors.New("not found")
	ErrPostNotFound  = errors.New("post not found")
	ErrInvalidPostID = errors.New("invalid post id")

	ErrMissingToken   = errors.New("missing bearer token")
	ErrInvalidToken   = errors.New("invalid or expired token")
	ErrInvalidPayload = errors.New("invalid token payload")

	// ErrMalformed is a body that is not JSON at all.
	ErrMalformed = errors.New("malformed JSON body")
	// ErrBodyRequired is JSON whose body is missing, not a string, or blank.
	ErrBodyRequired = errors.New("body is required")
	ErrBodyTooLong  = errors.New("body must be at most 500 characters")

	ErrInternal = errors.New("internal server error")
)
