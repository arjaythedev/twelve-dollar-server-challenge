package main

import (
	"os/exec"
	"path/filepath"
	"testing"
)

func TestCheckpointPolicy(t *testing.T) {
	binary := filepath.Join(t.TempDir(), "checkpoint-policy")
	cmd := exec.Command("cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
		"testdata/checkpoint_policy.c", "-o", binary)
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("compile policy contract: %v\n%s", err, output)
	}
	if output, err := exec.Command(binary).CombinedOutput(); err != nil {
		t.Fatalf("policy contract: %v\n%s", err, output)
	}
}
