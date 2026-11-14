package main

import (
	"bytes"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"
)

// Runs the real run() lifecycle in a fresh process: SQLite MEMSTATUS must be
// configured before the first connection, and signals must not reach go test.
func TestProfileRunLifecycle(t *testing.T) {
	if os.Getenv("GOAL_PROFILE_TEST_CHILD") == "1" {
		if err := run(); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		os.Exit(0)
	}
	_, seed := fixture(t, testSchema)
	for _, tc := range []struct {
		name      string
		enabled   bool
		badHeap   bool
		badPort   bool
		wantError bool
	}{
		{name: "disabled"},
		{name: "sigterm", enabled: true},
		{name: "heap_error", enabled: true, badHeap: true, wantError: true},
		{name: "startup_error", enabled: true, badPort: true, wantError: true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			cpu, heap := filepath.Join(dir, "cpu.pprof"), filepath.Join(dir, "heap.pprof")
			if tc.badHeap {
				if err := os.WriteFile(heap, nil, 0600); err != nil {
					t.Fatal(err)
				}
				heap = filepath.Join(heap, "blocked.pprof")
			}
			listener, err := net.Listen("tcp", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			port := fmt.Sprint(listener.Addr().(*net.TCPAddr).Port)
			listener.Close()
			if tc.badPort {
				port = "invalid"
			}
			executable, err := os.Executable()
			if err != nil {
				t.Fatal(err)
			}
			cmd := exec.Command(executable, "-test.run=^TestProfileRunLifecycle$")
			for _, entry := range os.Environ() {
				key, _, _ := strings.Cut(entry, "=")
				switch key {
				case "CPU_PROFILE", "HEAP_PROFILE", "SQLITE_PATH", "JWT_SECRET", "HOST", "PORT", "NETWORK_BACKEND", "GOMAXPROCS", "GOAL_PROFILE_TEST_CHILD", "GOAL_LLVM_PROFILE", "LLVM_PROFILE_FILE":
					continue
				}
				cmd.Env = append(cmd.Env, entry)
			}
			cmd.Env = append(cmd.Env, "GOAL_PROFILE_TEST_CHILD=1", "SQLITE_PATH="+seed, "JWT_SECRET=test secret", "HOST=127.0.0.1", "PORT="+port, "NETWORK_BACKEND=epoll", "GOMAXPROCS=2")
			raw := filepath.Join(dir, "sqlite.profraw")
			cmd.Env = append(cmd.Env, "GOAL_LLVM_PROFILE="+raw)
			if tc.enabled {
				cmd.Env = append(cmd.Env, "CPU_PROFILE="+cpu, "HEAP_PROFILE="+heap)
			}
			var output bytes.Buffer
			cmd.Stderr = &output
			if err := cmd.Start(); err != nil {
				t.Fatal(err)
			}
			done := make(chan error, 1)
			go func() { done <- cmd.Wait() }()
			t.Cleanup(func() { cmd.Process.Kill() })
			if !tc.badPort {
				deadline := time.Now().Add(5 * time.Second)
				for {
					conn, err := net.DialTimeout("tcp", "127.0.0.1:"+port, 100*time.Millisecond)
					if err == nil {
						conn.Close()
						break
					}
					if time.Now().After(deadline) {
						t.Fatal("child did not listen within 5 seconds")
					}
					time.Sleep(10 * time.Millisecond)
				}
				assertSQLiteProfileState(t, raw, false)
				if err := cmd.Process.Signal(syscall.SIGTERM); err != nil {
					t.Fatal(err)
				}
			}
			select {
			case err := <-done:
				if (err != nil) != tc.wantError {
					t.Fatalf("exit=%v wantError=%v\n%s", err, tc.wantError, output.String())
				}
			case <-time.After(5 * time.Second):
				cmd.Process.Kill()
				<-done
				t.Fatal("child did not exit within 5 seconds")
			}
			assertSQLiteProfileState(t, raw, true)
			if !tc.enabled {
				if strings.Contains(output.String(), "runtime_profile_summary") {
					t.Fatalf("profiling active by default: %s", output.String())
				}
				if _, err := os.Stat(cpu); !os.IsNotExist(err) {
					t.Fatalf("profiling created a file when disabled: %v", err)
				}
				return
			}
			assertProfileFlushed(t, cpu)
			if !tc.badHeap {
				assertProfileFlushed(t, heap)
			}
			if strings.Count(output.String(), `"kind":"runtime_profile_summary"`) != 1 {
				t.Fatalf("expected exactly one profile summary:\n%s", output.String())
			}
			if tc.badHeap && !strings.Contains(output.String(), `"profile_error"`) {
				t.Fatalf("missing heap error:\n%s", output.String())
			}
			if tc.badPort && !strings.Contains(output.String(), `invalid PORT "invalid"`) {
				t.Fatalf("startup error lost:\n%s", output.String())
			}
		})
	}
}
