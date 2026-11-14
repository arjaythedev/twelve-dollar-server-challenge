//go:build cgo && sqlite_llvm_pgo

package main

/*
#include <stdlib.h>
int goal_llvm_profile_export(const char *name);
*/
import "C"

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// Set only in generate builds. Each generation gets a fresh nonce, even if
// all compiler inputs match a previous training run.
var llvmTrainingID, llvmContractSHA256 string
var llvmProfilePath string

func fileSHA256(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func startSQLiteProfile() error {
	path := os.Getenv("GOAL_LLVM_PROFILE")
	if path == "" || !filepath.IsAbs(path) || filepath.Clean(path) != path || strings.ContainsAny(path, "%\n\r") {
		return fmt.Errorf("LLVM profile: GOAL_LLVM_PROFILE must be a clean absolute path without %% patterns")
	}
	if os.Getenv("LLVM_PROFILE_FILE") != "" {
		return fmt.Errorf("LLVM profile: LLVM_PROFILE_FILE must be unset")
	}
	if llvmTrainingID == "" || len(llvmContractSHA256) != 64 {
		return fmt.Errorf("LLVM profile: missing embedded training identity")
	}
	for _, p := range []string{path, path + ".json"} {
		if _, err := os.Lstat(p); !os.IsNotExist(err) {
			return fmt.Errorf("LLVM profile: output must be fresh: %s (%v)", p, err)
		}
	}
	lock, err := os.OpenFile(path+".lock", os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return fmt.Errorf("LLVM profile: reserve output: %w", err)
	}
	if _, err = lock.WriteString(llvmTrainingID + "\n"); err != nil {
		lock.Close()
		return fmt.Errorf("LLVM profile: write reservation: %w", err)
	}
	if err := lock.Close(); err != nil {
		return fmt.Errorf("LLVM profile: close reservation: %w", err)
	}
	llvmProfilePath = path
	return nil
}

// Only run()'s last SQLite-related defer calls this, after loop teardown and
// writer/checkpointer joins. Atomic increments protect concurrent collection;
// they do not make serializing counters while owners run safe.
func dumpSQLiteProfile() error {
	path := llvmProfilePath
	if path == "" {
		return fmt.Errorf("LLVM profile export: output was not initialized")
	}
	// O_EXCL rejects races and stale data. LLVM writes into this fresh empty file;
	// filenames contain no online-merge/continuous-mode expansion patterns.
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return fmt.Errorf("LLVM profile export: exclusive output: %w", err)
	}
	if err := f.Close(); err != nil {
		return fmt.Errorf("LLVM profile export: close output: %w", err)
	}
	name := C.CString(path)
	// compiler-rt retains this pointer. Keep this single training-only allocation
	// alive until process exit; automatic runtime initialization is disabled.
	if result := C.goal_llvm_profile_export(name); result != 0 {
		return fmt.Errorf("LLVM profile export: __llvm_profile_write_file returned %d for %s (see LLVM stderr)", int(result), path)
	}
	info, err := os.Stat(path)
	if err != nil || info.Size() == 0 {
		return fmt.Errorf("LLVM profile export: empty/missing raw data: %v", err)
	}
	// Sync the actual runtime output before publishing its success receipt.
	f, err = os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return fmt.Errorf("LLVM profile export: reopen: %w", err)
	}
	err = f.Sync()
	closeErr := f.Close()
	if err != nil || closeErr != nil {
		return fmt.Errorf("LLVM profile export: sync=%v close=%v", err, closeErr)
	}
	rawHash, err := fileSHA256(path)
	if err != nil {
		return fmt.Errorf("LLVM profile export: hash raw: %w", err)
	}
	executable, err := os.Executable()
	if err != nil {
		return fmt.Errorf("LLVM profile export: executable: %w", err)
	}
	binaryHash, err := fileSHA256(executable)
	if err != nil {
		return fmt.Errorf("LLVM profile export: hash binary: %w", err)
	}
	receipt := struct {
		TrainingID     string `json:"training_id"`
		ContractSHA256 string `json:"contract_sha256"`
		BinarySHA256   string `json:"binary_sha256"`
		RawSHA256      string `json:"raw_sha256"`
		RawSize        int64  `json:"raw_size"`
		ExportedAt     string `json:"exported_at"`
		PID            int    `json:"pid"`
	}{llvmTrainingID, llvmContractSHA256, binaryHash, rawHash, info.Size(), time.Now().UTC().Format(time.RFC3339Nano), os.Getpid()}
	f, err = os.OpenFile(path+".json", os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return fmt.Errorf("LLVM profile export: receipt: %w", err)
	}
	err = json.NewEncoder(f).Encode(receipt)
	if err == nil {
		err = f.Sync()
	}
	closeErr = f.Close()
	if err != nil || closeErr != nil {
		return fmt.Errorf("LLVM profile export: receipt write=%v close=%v", err, closeErr)
	}
	if err := os.Remove(path + ".lock"); err != nil {
		return fmt.Errorf("LLVM profile export: release reservation: %w", err)
	}
	fmt.Fprintf(os.Stderr, "sqlite_llvm_profile_export path=%s bytes=%d sha256=%s training_id=%s\n", path, info.Size(), rawHash, llvmTrainingID)
	llvmProfilePath = "" // a second call must fail instead of overwriting counters
	return nil
}
