SHELL := /bin/bash
.DEFAULT_GOAL := help
.NOTPARALLEL:

DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
JAR := $(DIR)/target/server.jar
SOURCES := $(DIR)/pom.xml $(shell find "$(DIR)/src")
COMPOSE := docker compose -f "$(DIR)/compose.yaml"
NATIVE_DOCKER := export DOCKER_DEFAULT_PLATFORM="$$(docker version --format '{{.Server.Os}}/{{.Server.Arch}}')" &&

# Where test-suite and benchmark run: docker (default), direct, or k8s.
MODE ?= docker
MAVEN ?= mvn
MVN_ARGS ?= --batch-mode --no-transfer-progress
JAVA ?= java
BUILD_IMAGES ?= true
WAIT_TIMEOUT ?= 120
# Where k6 runs in MODE=docker: docker (default) or local (this host).
BENCH_K6 ?= docker
# Image repository prefix that MODE=k8s pushes to, e.g. registry.example.com/owner.
K8S_REGISTRY ?=
# Existing namespace for MODE=k8s, and an optional image pull secret in it.
K8S_NAMESPACE ?= twelve-dollar-challenge
K8S_PULL_SECRET ?=
# true records each benchmark run with Java Flight Recorder (MODE=docker or k8s).
PROFILE ?= false
export BUILD_IMAGES WAIT_TIMEOUT BENCH_K6 K8S_REGISTRY K8S_NAMESPACE K8S_PULL_SECRET PROFILE

BENCH_WARMUP_VUS ?= 1000
BENCH_WARMUP_SECONDS ?= 120
BENCH_START_VUS ?= 2500
BENCH_MAX_VUS ?= 60000
BENCH_STEP ?= 250
BENCH_PROBE_SECONDS ?= 60
BENCH_CONFIRM_SECONDS ?= 300
BENCH_CONFIRM_RUNS ?= 1
BENCH_RAMP_SECONDS ?= 60
BENCH_BUDGET_SECONDS ?= 3600
BENCH_OUTPUT ?=
BENCH_ARGS ?=
REPORT_DIR ?= $(BENCH_OUTPUT)

ifeq ($(filter $(MODE),direct docker k8s),)
$(error MODE must be direct, docker, or k8s)
endif
# Direct mode runs the locally built JAR; Docker builds its own image.
MODE_DEPS := $(if $(filter direct,$(MODE)),$(JAR))

.PHONY: help all build test test-suite image run up down logs benchmark benchmark-smoke report

help:
	@printf '%s\n' \
	  'make build            Format check, Java tests, and target/server.jar with installed Maven' \
	  'make test             build, then the 42-check challenge suite' \
	  'make test-suite       Challenge suite on a fresh database (MODE=docker|direct|k8s)' \
	  'make benchmark        Warm-up, limit search, and reports (MODE=docker|direct|k8s)' \
	  'make benchmark-smoke  Short benchmark run that checks the tooling, not a score' \
	  'make report           Regenerate the latest report, or REPORT_DIR=<run directory>' \
	  'make all              build, test-suite, and benchmark' \
	  'make image            Build native app, seed, and test images' \
	  'make up / down / logs Regular Docker stack on LOCAL_PORT (default 3000)' \
	  'make run              Run the JAR; requires SQLITE_PATH and JWT_SECRET' \
	  '' \
	  'The app serves clients directly, without Nginx.' \
	  'MODE=docker runs the app in one container limited to 1 vCPU and 2 GiB,' \
	  'like the challenge VPS. BENCH_K6=local runs k6 on this host instead of in Docker.' \
	  'MODE=direct runs Java and k6 on this host and downloads missing tools' \
	  'to a temporary directory.' \
	  'MODE=k8s pushes images to K8S_REGISTRY and runs the app in a pod limited to' \
	  '1 vCPU and 2 GiB in namespace K8S_NAMESPACE of the current kubectl context,' \
	  'pulling with K8S_PULL_SECRET if set. k6 runs in its own pod on another node.' \
	  'PROFILE=true saves a Java Flight Recorder profile of each benchmark run' \
	  '(MODE=docker or k8s).' \
	  'Benchmark settings: BENCH_WARMUP_VUS=1000 BENCH_WARMUP_SECONDS=120' \
	  'BENCH_START_VUS=2500 BENCH_MAX_VUS=60000 BENCH_STEP=250 BENCH_PROBE_SECONDS=60' \
	  'BENCH_CONFIRM_SECONDS=300 BENCH_CONFIRM_RUNS=1 BENCH_RAMP_SECONDS=60' \
	  'BENCH_BUDGET_SECONDS=3600 BENCH_OUTPUT=<new directory> BENCH_ARGS="..."'

all:
	$(MAKE) -C "$(DIR)" build
	$(if $(filter docker,$(MODE)),$(MAKE) -C "$(DIR)" image)
	$(MAKE) -C "$(DIR)" test-suite BUILD_IMAGES=false
	$(MAKE) -C "$(DIR)" benchmark BUILD_IMAGES=false

build:
	cd "$(DIR)" && "$(MAVEN)" $(MVN_ARGS) clean verify

$(JAR): $(SOURCES)
	cd "$(DIR)" && "$(MAVEN)" $(MVN_ARGS) verify

test: build
	$(MAKE) -C "$(DIR)" test-suite

test-suite: $(MODE_DEPS)
	bash "$(DIR)/scripts/$(MODE).sh" test

benchmark: $(MODE_DEPS)
	bash "$(DIR)/scripts/$(MODE).sh" benchmark \
	  --warmup-vus "$(BENCH_WARMUP_VUS)" --warmup-seconds "$(BENCH_WARMUP_SECONDS)" \
	  --start-vus "$(BENCH_START_VUS)" --max-vus "$(BENCH_MAX_VUS)" --step "$(BENCH_STEP)" \
	  --probe-seconds "$(BENCH_PROBE_SECONDS)" --confirm-seconds "$(BENCH_CONFIRM_SECONDS)" \
	  --confirm-runs "$(BENCH_CONFIRM_RUNS)" --ramp-seconds "$(BENCH_RAMP_SECONDS)" \
	  --budget-seconds "$(BENCH_BUDGET_SECONDS)" \
	  $(if $(BENCH_OUTPUT),--output "$(BENCH_OUTPUT)") $(BENCH_ARGS)

benchmark-smoke:
	$(MAKE) -C "$(DIR)" benchmark BENCH_WARMUP_VUS=50 BENCH_WARMUP_SECONDS=8 \
	  BENCH_START_VUS=50 BENCH_MAX_VUS=100 BENCH_STEP=50 BENCH_PROBE_SECONDS=8 \
	  BENCH_CONFIRM_SECONDS=10 BENCH_RAMP_SECONDS=1 BENCH_BUDGET_SECONDS=600

report:
	python3 "$(DIR)/benchmark_report.py" $(if $(REPORT_DIR),"$(REPORT_DIR)")

image:
	$(NATIVE_DOCKER) $(COMPOSE) --profile test build server seed test

up:
	$(NATIVE_DOCKER) $(COMPOSE) up --build -d --wait --wait-timeout "$(WAIT_TIMEOUT)"

down:
	$(COMPOSE) down

logs:
	$(COMPOSE) logs --follow server seed

run: $(JAR)
	JAVA="$(JAVA)" exec bash "$(DIR)/start.sh"
