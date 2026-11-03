package main

import (
	"bytes"
	"encoding/json"
	"time"
	"unicode/utf8"
)

type bodyErr int

const (
	errNone bodyErr = iota
	errMalformed
	errBodyRequired
	errBodyTooLong
)

// parseBodyField parses {"body":"..."} and returns the trimmed body.
//
// Order matches SPEC:
//  1. invalid JSON               -> errMalformed
//  2. body missing/not a string  -> errBodyRequired
//     or empty after trimming
//  3. > 500 chars after trimming -> errBodyTooLong
func parseBodyField(raw []byte) (string, bodyErr) {
	// Parse top-level JSON object generically so we can distinguish a missing
	// key, a non-string value, and a malformed document.
	var obj map[string]json.RawMessage
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.UseNumber()
	if err := dec.Decode(&obj); err != nil {
		return "", errMalformed
	}
	// Reject trailing garbage after the first JSON value.
	if dec.More() {
		return "", errMalformed
	}

	bv, ok := obj["body"]
	if !ok {
		return "", errBodyRequired
	}
	var s string
	if err := json.Unmarshal(bv, &s); err != nil {
		// body present but not a JSON string (number, object, null, ...)
		return "", errBodyRequired
	}
	trimmed := trimSpace(s)
	if trimmed == "" {
		return "", errBodyRequired
	}
	// "at most 500 characters" — count Unicode code points, not bytes.
	if utf8.RuneCountInString(trimmed) > 500 {
		return "", errBodyTooLong
	}
	return trimmed, errNone
}

// trimSpace trims leading/trailing ASCII whitespace. The test uses spaces; we
// match Go's strings.TrimSpace semantics for Unicode whitespace too.
func trimSpace(s string) string {
	start := 0
	for start < len(s) && isSpace(s[start]) {
		start++
	}
	end := len(s)
	for end > start && isSpace(s[end-1]) {
		end--
	}
	return s[start:end]
}

func isSpace(c byte) bool {
	switch c {
	case ' ', '\t', '\n', '\r', '\v', '\f':
		return true
	}
	return false
}

func timeSinceStartSeconds() float64 {
	return time.Since(startTime).Seconds()
}
