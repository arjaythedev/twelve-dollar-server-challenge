package main

import (
	"errors"
	"strings"

	"github.com/golang-jwt/jwt/v5"
)

type identity struct {
	userID   int64
	username string
}

func authenticate(header string, secret []byte) (identity, error) {
	if !strings.HasPrefix(header, "Bearer ") {
		return identity{}, errors.New("missing bearer token")
	}
	token, err := jwt.Parse(strings.TrimPrefix(header, "Bearer "), func(token *jwt.Token) (any, error) { return secret, nil }, jwt.WithValidMethods([]string{"HS256"}), jwt.WithExpirationRequired())
	if err != nil || !token.Valid {
		return identity{}, errors.New("invalid or expired token")
	}
	claims := token.Claims.(jwt.MapClaims)
	sub, ok := claims["sub"].(string)
	if !ok {
		return identity{}, errors.New("invalid token payload")
	}
	id, err := parsePostID(sub)
	username, ok := claims["username"].(string)
	if err != nil || !ok {
		return identity{}, errors.New("invalid token payload")
	}
	return identity{id, username}, nil
}
