package main

import (
	"errors"
	"fmt"

	"github.com/golang-jwt/jwt/v5"
)

var (
	errInvalidToken   = errors.New("invalid or expired token")
	errInvalidPayload = errors.New("invalid token payload")
)

type User struct {
	ID       int64
	Username string
}

type Verifier struct {
	secret []byte
	parser *jwt.Parser
}

func newVerifier(secret string) (*Verifier, error) {
	if secret == "" {
		return nil, errors.New("auth: empty secret")
	}
	return &Verifier{
		secret: []byte(secret),
		parser: jwt.NewParser(jwt.WithValidMethods([]string{"HS256"}), jwt.WithExpirationRequired()),
	}, nil
}

func (v *Verifier) Verify(token string) (User, error) {
	claims := jwt.MapClaims{}
	if _, err := v.parser.ParseWithClaims(token, claims, v.key); err != nil {
		return User{}, fmt.Errorf("%w: %v", errInvalidToken, err)
	}

	sub, ok := claims["sub"].(string)
	if !ok {
		return User{}, errInvalidPayload
	}
	id, ok := parseID(sub)
	if !ok {
		return User{}, errInvalidPayload
	}
	name, ok := claims["username"].(string)
	if !ok {
		return User{}, errInvalidPayload
	}
	return User{ID: id, Username: name}, nil
}

func (v *Verifier) key(*jwt.Token) (any, error) { return v.secret, nil }
