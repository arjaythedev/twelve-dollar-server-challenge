// SPDX-License-Identifier: MIT
package main

import (
	"encoding/json"
	"strings"
	"testing"
	"unicode/utf8"
)

func jsonObject(data []byte) bool {
	var object map[string]any
	return utf8.Valid(data) && json.Unmarshal(data, &object) == nil && object != nil
}

type claimMutation struct {
	field, value string
	want         result
}

const inputString = "<input string>"

// Literal expectations are independent of both implementations. All other
// claims remain valid, so a rejection cannot be hidden by an earlier bad claim.
// Empty value deletes a field; inputString inserts arbitrary escaped UTF-8 data.
func claimMutations() []claimMutation {
	return []claimMutation{
		{"alg", `"HS256"`, result{1, "fixture", ""}},
		{"alg", `"none"`, result{err: invalid}},
		{"alg", `null`, result{err: invalid}},
		{"alg", `1`, result{err: invalid}},
		{"alg", `{}`, result{err: invalid}},
		{"alg", `[]`, result{err: invalid}},
		{"alg", "", result{err: invalid}},
		{"alg", inputString, result{err: invalid}},
		{"exp", `4102444800`, result{1, "fixture", ""}},
		{"exp", `4102444800.5`, result{1, "fixture", ""}},
		{"exp", `4.1024448e9`, result{1, "fixture", ""}},
		{"exp", `1`, result{err: invalid}},
		{"exp", `null`, result{err: invalid}},
		{"exp", `false`, result{err: invalid}},
		{"exp", `{}`, result{err: invalid}},
		{"exp", `[]`, result{err: invalid}},
		{"exp", "", result{err: invalid}},
		{"exp", inputString, result{err: invalid}},
		{"nbf", `0`, result{1, "fixture", ""}},
		{"nbf", `1.25`, result{1, "fixture", ""}},
		{"nbf", `4102444800`, result{err: invalid}},
		{"nbf", `null`, result{err: invalid}},
		{"nbf", `false`, result{err: invalid}},
		{"nbf", `{}`, result{err: invalid}},
		{"nbf", `[]`, result{err: invalid}},
		{"nbf", "", result{1, "fixture", ""}},
		{"nbf", inputString, result{err: invalid}},
		{"sub", `"1"`, result{1, "fixture", ""}},
		{"sub", `"001"`, result{1, "fixture", ""}},
		{"sub", `"9007199254740991"`, result{9007199254740991, "fixture", ""}},
		{"sub", `"0"`, result{err: payloadError}},
		{"sub", `"-1"`, result{err: payloadError}},
		{"sub", `"9007199254740992"`, result{err: payloadError}},
		{"sub", `"9223372036854775808"`, result{err: payloadError}},
		{"sub", `1`, result{err: payloadError}},
		{"sub", `null`, result{err: payloadError}},
		{"sub", `{}`, result{err: payloadError}},
		{"sub", `[]`, result{err: payloadError}},
		{"sub", "", result{err: payloadError}},
		{"sub", inputString, result{err: payloadError}},
		{"username", `"fixture"`, result{1, "fixture", ""}},
		{"username", `""`, result{1, "", ""}},
		{"username", `null`, result{err: payloadError}},
		{"username", `1`, result{err: payloadError}},
		{"username", `false`, result{err: payloadError}},
		{"username", `{}`, result{err: payloadError}},
		{"username", `[]`, result{err: payloadError}},
		{"username", "", result{err: payloadError}},
		{"username", inputString, result{1, "", ""}},
	}
}

func compareStructured(t *testing.T, mutation claimMutation, input string) result {
	t.Helper()
	head := map[string]json.RawMessage{"alg": json.RawMessage(`"HS256"`)}
	body := map[string]json.RawMessage{
		"exp": json.RawMessage(`4102444800`), "nbf": json.RawMessage(`0`),
		"sub": json.RawMessage(`"1"`), "username": json.RawMessage(`"fixture"`),
	}
	target := body
	if mutation.field == "alg" {
		target = head
	}
	want := mutation.want
	if mutation.value == "" {
		delete(target, mutation.field)
	} else if mutation.value == inputString {
		value := strings.ToValidUTF8(input, "\ufffd")
		if mutation.field == "username" {
			want.name = value
		} else {
			// Keep arbitrary strings invalid for alg/numeric claims/sub.
			value = "invalid:" + value
		}
		encoded, err := json.Marshal(value)
		if err != nil {
			t.Fatal(err)
		}
		target[mutation.field] = encoded
	} else {
		target[mutation.field] = json.RawMessage(mutation.value)
	}
	h, headErr := json.Marshal(head)
	b, bodyErr := json.Marshal(body)
	if headErr != nil || bodyErr != nil || !jsonObject(h) || !jsonObject(b) {
		t.Fatalf("structured %s mutation did not produce JSON objects: %v, %v", mutation.field, headErr, bodyErr)
	}
	got := call(authenticateSource, secret, token(string(h), string(b)))
	if got != want {
		t.Fatalf("structured %s=%q: got %+v, literal expectation %+v", mutation.field, mutation.value, got, want)
	}
	return got
}

func TestStructuredClaimMutations(t *testing.T) {
	coverage := make(map[string][2]int)
	for _, mutation := range claimMutations() {
		got := compareStructured(t, mutation, "Żółć \x00\xff\n☃")
		hits := coverage[mutation.field]
		if got.err == "" {
			hits[0]++
		} else {
			hits[1]++
		}
		coverage[mutation.field] = hits
	}
	for _, field := range []string{"alg", "exp", "nbf", "sub", "username"} {
		hits := coverage[field]
		if hits[0] == 0 || hits[1] == 0 {
			t.Errorf("%s: signed JSON needs accepted and rejected controls: %v", field, hits)
		}
	}
}
