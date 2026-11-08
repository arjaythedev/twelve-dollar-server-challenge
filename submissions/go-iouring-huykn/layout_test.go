package main

import (
	"reflect"
	"testing"
	"unsafe"
)

// The submission targets 64-bit Linux. Keep the reviewed struct sizes from
// regressing when a scalar is added between pointer-bearing fields.
func TestStructSizes(t *testing.T) {
	if unsafe.Sizeof(uintptr(0)) != 8 {
		t.Skip("layout expectations apply to 64-bit targets")
	}
	types := map[string]reflect.Type{
		"app":        reflect.TypeOf(app{}),
		"connection": reflect.TypeOf(connection{}),
		"byteReader": reflect.TypeOf(byteReader{}),
		"byteValue":  reflect.TypeOf(byteValue{}),
		"request":    reflect.TypeOf(request{}),
		"server":     reflect.TypeOf(server{}),
		"verifier":   reflect.TypeOf(verifier{}),
	}
	want := map[string]uintptr{
		"app": 64, "connection": 72, "byteReader": 32, "byteValue": 32,
		"request": 104, "server": 72, "verifier": 96,
	}
	for name, typ := range types {
		if got := typ.Size(); got != want[name] {
			t.Errorf("%s: size %d, want %d", name, got, want[name])
		}
		t.Logf("%s: size=%d align=%d", name, typ.Size(), typ.Align())
	}
}
