//go:build !sqlite_llvm_pgo

package main

import (
	"os"
	"testing"
)

func assertSQLiteProfileState(t *testing.T, path string, _ bool) {
	t.Helper()
	for _, suffix := range []string{"", ".json", ".lock"} {
		if _, err := os.Stat(path + suffix); !os.IsNotExist(err) {
			t.Fatalf("non-training binary wrote a profile artifact %s: %v", suffix, err)
		}
	}
}
