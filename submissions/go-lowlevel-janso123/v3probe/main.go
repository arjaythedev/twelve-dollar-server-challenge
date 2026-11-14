// Command v3probe exits 0 only if this CPU runs x86-64-v3 code. The Makefile builds it with the same
// GOAMD64 and C flags as the server: the Go runtime aborts a GOAMD64=v3 binary on an older CPU before
// main runs, and the C side asks the CPU directly.
package main

// #cgo CFLAGS: -O2
// static int cpu_v3(void) {
//     __builtin_cpu_init();
//     return __builtin_cpu_supports("x86-64-v3");
// }
import "C"

import "os"

func main() {
	if C.cpu_v3() == 0 {
		os.Exit(1)
	}
}
