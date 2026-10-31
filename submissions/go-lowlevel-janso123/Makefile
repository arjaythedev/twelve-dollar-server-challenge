# LLVM_BIN and GO may select installed toolchains of the pinned versions.
# Default: bundled Go PGO + SQLite PGO + ThinLTO; SQLite PGO requires v3.
# Training is never enabled. Go PGO, SQLite PGO and ThinLTO toggle independently.
.DEFAULT_GOAL := all
GO ?= /usr/local/go/bin/go
LLVM_BIN ?= /opt/llvm-23.1.3/bin
export GO LLVM_BIN

.PHONY: all baseline pgo nonpgo nolto test test-pgo test-build thinlto test-thinlto go-pgo test-go-pgo no-go-pgo test-no-go-pgo clean
all:
	./build.sh

# Explicit v1 without either Go or SQLite PGO; ThinLTO remains enabled.
baseline:
	./build.sh --baseline

pgo:
	./build.sh --pgo

# Disable only SQLite PGO with CPU-probed v3/v1; keep Go PGO and ThinLTO.
nonpgo:
	./build.sh --no-pgo

# Go and SQLite PGO without ThinLTO; SQLite PGO still requires v3.
nolto:
	./build.sh --no-thinlto

test:
	./build.sh --test

test-pgo:
	./build.sh --pgo --test

thinlto:
	./build.sh --pgo --thinlto

test-thinlto:
	./build.sh --pgo --thinlto --test

# Explicit bundled Go PGO; equivalent to the default optimization settings.
go-pgo:
	./build.sh --go-pgo profiles/go.pprof --go-pgo-sha256 cffbedaaa8f7208e6cce79844a858e8ae2f08a3a14079cdc345f0968e63eb6ff

test-go-pgo:
	./build.sh --test --go-pgo profiles/go.pprof --go-pgo-sha256 cffbedaaa8f7208e6cce79844a858e8ae2f08a3a14079cdc345f0968e63eb6ff

# Disable only Go PGO; retain SQLite PGO + ThinLTO.
no-go-pgo:
	./build.sh --no-go-pgo

test-no-go-pgo:
	./build.sh --no-go-pgo --test

# No LLVM, network, or Linux compilation; real profile checks use local Go if present.
test-build:
	python3 -m unittest discover -s tests -p test_build.py -v

clean:
	rm -rf bin build
