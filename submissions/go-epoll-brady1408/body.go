package main

import (
	"bytes"
	"encoding/json"
	"strings"
	"unicode/utf8"
)

const (
	maxPostBodyChars = 500
	errBodyRequired  = "body is required"
	errBodyTooLong   = "body must be at most 500 characters"
	errMalformedJSON = "malformed JSON body"
)

type createRequest struct {
	Body json.RawMessage `json:"body"`
}

// validateBody applies SPEC.md's rules for POST /posts and returns the trimmed
// body, or the error message for the 400 response.
func validateBody(raw []byte) (string, string) {
	if len(bytes.TrimSpace(raw)) == 0 {
		return "", errBodyRequired // an empty body is "required", not malformed (Express semantics)
	}
	var req createRequest
	if err := json.Unmarshal(raw, &req); err != nil {
		return "", errMalformedJSON
	}
	if len(req.Body) == 0 || req.Body[0] != '"' {
		return "", errBodyRequired
	}
	var body string
	if err := json.Unmarshal(req.Body, &body); err != nil {
		return "", errMalformedJSON
	}
	body = strings.TrimSpace(body)
	if body == "" {
		return "", errBodyRequired
	}
	if utf8.RuneCountInString(body) > maxPostBodyChars {
		return "", errBodyTooLong
	}
	return body, ""
}

// parsePostID accepts a non-empty run of ASCII digits with a positive value
// that fits in int64. Anything else (signs, dots, spaces, overflow) is invalid.
func parsePostID(seg []byte) (int64, bool) {
	if len(seg) == 0 || len(seg) > 19 {
		return 0, false
	}
	var n int64
	for _, c := range seg {
		if c < '0' || c > '9' {
			return 0, false
		}
		d := int64(c - '0')
		if n > (1<<63-1-d)/10 {
			return 0, false
		}
		n = n*10 + d
	}
	return n, n > 0
}
