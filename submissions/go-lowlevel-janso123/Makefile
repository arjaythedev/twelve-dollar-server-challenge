# Builds bin/server from app/.
#
# On x86-64 it first checks that this CPU can run x86-64-v3 code (AVX2, BMI2, FMA, ...): a tiny probe
# program is built with the same flags and run once. If it runs, the server is built with GOAMD64=v3 and
# -march=x86-64-v3 for the C code (SQLite and the feed helper). If anything about that fails, the build
# falls back to the baseline flags, so v3 can only ever make the build faster, never break it.
#
#   make            build bin/server (v3 if possible)
#   make baseline   build bin/server without v3
#   make clean

GO      ?= /usr/local/go/bin/go
TAGS    := cgo_sqlite
BIN     := bin/server
GOARCH  := $(shell $(GO) env GOARCH)

export CGO_ENABLED := 1
export GOTOOLCHAIN := local
export GOFLAGS     := -mod=readonly -trimpath

BASE_CFLAGS := -O2
V3_CFLAGS   := -O2 -march=x86-64-v3

.PHONY: all baseline clean

all:
	@mkdir -p bin build
	@if [ "$(GOARCH)" = amd64 ] && $(MAKE) --no-print-directory v3-probe; then \
		echo "build: CPU runs x86-64-v3, building with GOAMD64=v3"; \
		if GOAMD64=v3 CGO_CFLAGS="$(V3_CFLAGS)" $(GO) build -C app -tags $(TAGS) -o ../$(BIN) . ; then \
			exit 0; \
		fi; \
		echo "build: v3 build failed, falling back to baseline"; \
	else \
		echo "build: x86-64-v3 not available here, building baseline"; \
	fi; \
	$(MAKE) --no-print-directory baseline

baseline:
	@mkdir -p bin
	GOAMD64=v1 CGO_CFLAGS="$(BASE_CFLAGS)" $(GO) build -C app -tags $(TAGS) -o ../$(BIN) .

# Succeeds only if a program built with the v3 flags (Go and C) actually runs on this CPU and the CPU
# reports x86-64-v3 support. The Go runtime refuses to start a GOAMD64=v3 binary on an older CPU.
.PHONY: v3-probe
v3-probe:
	@mkdir -p build
	@GOAMD64=v3 CGO_CFLAGS="$(V3_CFLAGS)" $(GO) build -C v3probe -o ../build/v3probe . >/dev/null 2>&1 \
		&& ./build/v3probe >/dev/null 2>&1

clean:
	rm -rf bin build
