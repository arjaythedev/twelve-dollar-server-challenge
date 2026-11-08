package main

import (
	"bytes"
	"testing"
	"unicode/utf8"
)

func TestWalkObject(t *testing.T) {
	for _, tc := range []struct {
		doc, body  string
		valid, obj bool
		kind       uint8
	}{
		{`{"body":"hello"}`, "hello", true, true, byteString},
		{`{"body":"first","b\u006fdy":"last"}`, "last", true, true, byteString},
		{`{"body":"first","body":null}`, "", true, true, byteOther},
		{`{"body":"\ud83d\ude00 \ud800"}`, "😀 �", true, true, byteString},
		{`{"ignored":[true,false,null,{"n":-1.25e+3}],"body":"x"}`, "x", true, true, byteString},
		{`{"ignored":{"body":"nested"}}`, "", true, true, byteMissing},
		{`[1,2,3]`, "", true, false, byteMissing},
		{`null`, "", true, false, byteMissing},
		{`{"body":"x","ignored":[1,]}`, "", false, true, byteMissing},
		{`{"body":"x"} trailing`, "", false, true, byteMissing},
		{`{"body":"\u00xz"}`, "", false, true, byteMissing},
		{`{"body":"\q"}`, "", false, true, byteMissing},
		{`{"body":01}`, "", false, true, byteMissing},
		{`{"body":1.}`, "", false, true, byteMissing},
		{`{"body":1e+}`, "", false, true, byteMissing},
		{`{"body":"x",}`, "", false, true, byteMissing},
		{"{\"body\":\"\xff\"}", "", false, false, byteMissing},
	} {
		t.Run(tc.doc, func(t *testing.T) {
			var body byteValue
			valid, obj := walkObject([]byte(tc.doc), nil, func(key []byte, value byteValue) {
				if string(key) == "body" {
					body = value
				}
			})
			if valid != tc.valid || obj != tc.obj {
				t.Fatalf("valid=%v object=%v", valid, obj)
			}
			if !valid {
				return
			}
			if body.kind != tc.kind {
				t.Fatalf("kind=%d", body.kind)
			}
			if body.kind == byteString {
				got := body.raw
				if body.escaped {
					got = unescape(got, nil)
				}
				if string(got) != tc.body {
					t.Fatalf("body=%q", got)
				}
			}
		})
	}
	for _, depth := range []int{64, 65} {
		doc := append(bytes.Repeat([]byte{'['}, depth), '0')
		doc = append(doc, bytes.Repeat([]byte{']'}, depth)...)
		valid, _ := walkObject(doc, nil, nil)
		if valid != (depth == 64) {
			t.Fatalf("depth %d: valid=%v", depth, valid)
		}
	}
}

func FuzzWalkObject(f *testing.F) {
	for _, s := range []string{`{}`, `{"body":"x"}`, `{"body":"\ud83d\ude00"}`, `{"exp":1e+9,"unknown":[null,false]}`, `{"x":`, `"\\"`} {
		f.Add([]byte(s))
	}
	f.Fuzz(func(t *testing.T, input []byte) {
		if len(input) > 16384 {
			return
		}
		walkObject(input, nil, func(_ []byte, value byteValue) {
			if value.kind == byteString && value.escaped && !utf8.Valid(unescape(value.raw, nil)) {
				t.Fatal("decoded string is invalid UTF-8")
			}
		})
	})
}
