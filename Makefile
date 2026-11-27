# OpenBench-compatible convenience Makefile for POSIX hosts (Linux x86-64, macOS arm64).
# Windows x64 builds use CMake + Ninja directly (`cmake --preset windows-release`).

UNAME_S := $(shell uname -s)
BUILD_DIR ?= build/make-release
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
EXE ?=
EVALFILE ?= $(wildcard artifacts/networks/koi-v9-hm10-h256-official.nnue)
BENCH_DEPTH ?= 10
BENCH_HASH ?= 16
BENCH_THREADS ?= 1
GENERATOR := $(if $(shell command -v ninja 2>/dev/null),-G Ninja,)

ifeq ($(UNAME_S),Darwin)
  CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=Release -DKOI_BUILD_MODULES=OFF -DKOI_STATIC_RUNTIME=OFF
else
  CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=Release -DKOI_BUILD_MODULES=OFF
endif

.PHONY: all release bench test docs clean help

all: release

release:
	cmake -S . -B "$(BUILD_DIR)" $(GENERATOR) $(CMAKE_FLAGS)
	cmake --build "$(BUILD_DIR)" --config Release --target koi_engine -j "$(JOBS)"
	@if [ -n "$(EXE)" ]; then cp "$(BUILD_DIR)/koi-engine" "$(EXE)" && chmod +x "$(EXE)"; fi

bench: release
	@if [ -n "$(EVALFILE)" ]; then \
	  KOI_NNUE_PATH="$(EVALFILE)" "$(BUILD_DIR)/koi-engine" bench "$(BENCH_DEPTH)" "$(BENCH_HASH)" "$(BENCH_THREADS)"; \
	else \
	  "$(BUILD_DIR)/koi-engine" bench "$(BENCH_DEPTH)" "$(BENCH_HASH)" "$(BENCH_THREADS)"; \
	fi

test:
	cmake -S . -B "$(BUILD_DIR)" $(GENERATOR) $(CMAKE_FLAGS)
	cmake --build "$(BUILD_DIR)" --config Release -j "$(JOBS)"
	ctest --test-dir "$(BUILD_DIR)" -C Release -j "$(JOBS)" --output-on-failure

docs:
	python3 tools/docs/prepare_pages.py
	mkdocs build --strict

clean:
	rm -rf "$(BUILD_DIR)"

help:
	@echo "Koi Makefile targets (Linux x86-64 / macOS arm64):"
	@echo "  make [EXE=<path>]                      Build koi-engine Release (copies binary to EXE for OpenBench)"
	@echo "  make bench [EVALFILE=...]              Build and run deterministic OpenBench benchmark"
	@echo "  make test                              Build full suite and run CTest"
	@echo "  make docs                              Build MkDocs Material site strictly"
	@echo "  make clean                             Remove $(BUILD_DIR)"
