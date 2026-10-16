package domain

import "context"

// Principal is who an authenticated request acts as, read from its token.
// The username is used as the token states it; nothing looks it up.
type Principal struct {
	UserID   int64
	Username string
}

// authState is what the http layer learned from a request's Authorization
// header: a principal, or why there is none.
type authState struct {
	p   Principal
	err error
}

type authKey struct{}

// WithAuth records the outcome of checking the request's token. Requests with
// no Authorization header carry nothing, which reads as ErrMissingToken.
func WithAuth(ctx context.Context, p Principal, err error) context.Context {
	return context.WithValue(ctx, authKey{}, &authState{p, err})
}

// PrincipalFrom is the request's principal, or the reason it has none.
func PrincipalFrom(ctx context.Context) (Principal, error) {
	s, ok := ctx.Value(authKey{}).(*authState)
	if !ok {
		return Principal{}, ErrMissingToken
	}
	return s.p, s.err
}
