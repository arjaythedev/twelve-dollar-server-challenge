//go:build cgo && sqlite_llvm_pgo

package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// A fresh subprocess exercises the actual CGO exporter. Native fixture tests
// reuse these exact hook files without importing the Linux application.
func TestLLVMProfileExport(t *testing.T) {
	if mode := os.Getenv("GOAL_LLVM_HOOK_TEST_CHILD"); mode != "" {
		if err := startSQLiteProfile(); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		if mode == "no_dump" {
			os.Exit(0) // Go exit must not invoke a C constructor/atexit dump.
		}
		if mode == "collision" {
			if err := os.WriteFile(os.Getenv("GOAL_LLVM_PROFILE"), []byte("do not overwrite"), 0600); err != nil {
				panic(err)
			}
		}
		if mode == "output_error" {
			// Force the checked file-creation failure; the separate native fixture
			// tests the actual compiler-rt nonzero return path.
			if err := os.Mkdir(os.Getenv("GOAL_LLVM_PROFILE"), 0700); err != nil {
				panic(err)
			}
		}
		if err := dumpSQLiteProfile(); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		os.Exit(0)
	}
	for _, mode := range []string{"export", "no_dump", "collision", "output_error", "bad_path"} {
		t.Run(mode, func(t *testing.T) {
			dir := t.TempDir()
			path := filepath.Join(dir, "sqlite.profraw")
			if mode == "bad_path" {
				path = filepath.Join(dir, "%m.profraw")
			}
			executable, err := os.Executable()
			if err != nil {
				t.Fatal(err)
			}
			runChild := func() ([]byte, error) {
				cmd := exec.Command(executable, "-test.run=^TestLLVMProfileExport$")
				cmd.Dir = dir
				for _, entry := range os.Environ() {
					key, _, _ := strings.Cut(entry, "=")
					if key != "GOAL_LLVM_PROFILE" && key != "GOAL_LLVM_HOOK_TEST_CHILD" && key != "LLVM_PROFILE_FILE" {
						cmd.Env = append(cmd.Env, entry)
					}
				}
				cmd.Env = append(cmd.Env, "GOAL_LLVM_PROFILE="+path, "GOAL_LLVM_HOOK_TEST_CHILD="+mode)
				return cmd.CombinedOutput()
			}
			output, err := runChild()
			wantError := mode == "collision" || mode == "output_error" || mode == "bad_path"
			if (err != nil) != wantError {
				t.Fatalf("exit=%v wantError=%v\n%s", err, wantError, output)
			}
			if _, err := os.Stat(filepath.Join(dir, "default.profraw")); !os.IsNotExist(err) {
				t.Fatalf("automatic runtime wrote default.profraw: %v", err)
			}
			if wantError {
				if !bytes.Contains(output, []byte("LLVM profile")) {
					t.Fatalf("missing export diagnostic: %s", output)
				}
				if mode == "collision" {
					data, _ := os.ReadFile(path)
					if string(data) != "do not overwrite" {
						t.Fatalf("overwrote a collided profile: %q", data)
					}
				}
				if _, err := os.Stat(path + ".json"); !os.IsNotExist(err) {
					t.Fatalf("failed export published a receipt: %v", err)
				}
				return
			}
			if mode == "no_dump" {
				if _, err := os.Stat(path); !os.IsNotExist(err) {
					t.Fatalf("Go exit wrote raw data before explicit dump: %v", err)
				}
				return
			}
			var receipt struct {
				TrainingID     string `json:"training_id"`
				ContractSHA256 string `json:"contract_sha256"`
				BinarySHA256   string `json:"binary_sha256"`
				RawSHA256      string `json:"raw_sha256"`
				RawSize        int64  `json:"raw_size"`
			}
			data, err := os.ReadFile(path + ".json")
			if err != nil {
				t.Fatal(err)
			}
			if err := json.Unmarshal(data, &receipt); err != nil {
				t.Fatal(err)
			}
			rawHash, err := fileSHA256(path)
			if err != nil {
				t.Fatal(err)
			}
			binaryHash, err := fileSHA256(executable)
			if err != nil {
				t.Fatal(err)
			}
			if receipt.TrainingID != llvmTrainingID || receipt.ContractSHA256 != llvmContractSHA256 || receipt.BinarySHA256 != binaryHash || receipt.RawSHA256 != rawHash || receipt.RawSize <= 0 {
				t.Fatalf("incorrect receipt: %+v", receipt)
			}
			if _, err := os.Stat(path + ".lock"); !os.IsNotExist(err) {
				t.Fatalf("successful export retained its reservation: %v", err)
			}
			before, _ := os.ReadFile(path)
			output, err = runChild()
			if err == nil || !bytes.Contains(output, []byte("output must be fresh")) {
				t.Fatalf("accepted stale raw data: err=%v\n%s", err, output)
			}
			after, _ := os.ReadFile(path)
			if !bytes.Equal(before, after) {
				t.Fatal("reused/modified stale raw profile")
			}
		})
	}
}

// The inherited real-process lifecycle calls this before shutdown and after
// SIGTERM/startup errors/Go heap-profile errors. Only a successful post-join
// export may produce raw data and its receipt.
func assertSQLiteProfileState(t *testing.T, path string, stopped bool) {
	t.Helper()
	if !stopped {
		for _, suffix := range []string{"", ".json"} {
			if _, err := os.Stat(path + suffix); !os.IsNotExist(err) {
				t.Fatalf("LLVM profile exists before SQLite owners joined: %v", err)
			}
		}
		return
	}
	info, err := os.Stat(path)
	if err != nil || info.Size() == 0 {
		t.Fatalf("no raw LLVM data after graceful run: %v", err)
	}
	data, err := os.ReadFile(path + ".json")
	if err != nil {
		t.Fatal(err)
	}
	var receipt map[string]any
	if err := json.Unmarshal(data, &receipt); err != nil {
		t.Fatal(err)
	}
	hash, err := fileSHA256(path)
	if err != nil || receipt["raw_sha256"] != hash || receipt["training_id"] != llvmTrainingID {
		t.Fatalf("invalid LLVM export receipt: %s error=%v", data, err)
	}
	if _, err := os.Stat(path + ".lock"); !os.IsNotExist(err) {
		t.Fatalf("export did not release reservation: %v", err)
	}
	profdata := os.Getenv("GOAL_LLVM_PROFDATA")
	if profdata == "" {
		t.Fatal("GOAL_LLVM_PROFDATA required to validate raw LLVM counters")
	}
	merged := filepath.Join(t.TempDir(), "sqlite.profdata")
	if output, err := exec.Command(profdata, "merge", "--failure-mode=any", "-o", merged, path).CombinedOutput(); err != nil {
		t.Fatalf("LLVM rejected raw profile: %v\n%s", err, output)
	}
	output, err := exec.Command(profdata, "show", "--all-functions", "--counts", merged).CombinedOutput()
	if err != nil || !bytes.Contains(output, []byte("Instrumentation level: IR")) || !bytes.Contains(output, []byte("sqlite3_step")) || bytes.Contains(output, []byte("Maximum function count: 0\n")) {
		t.Fatalf("invalid/unexecuted LLVM SQLite counters: %v\n%s", err, output)
	}
}
