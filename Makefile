# OpenBench-compatible convenience Makefile for POSIX hosts (Linux x86-64, macOS arm64).
# Windows x64 builds use CMake + Ninja directly (`cmake --preset windows-release`).

UNAME_S := $(shell uname -s)
BUILD_DIR ?= build/make-release
PGO_GEN_DIR := $(BUILD_DIR)-pgo-gen
PGO_DATA_DIR := $(BUILD_DIR)-pgo-data
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
EXE ?=
EVALFILE ?= $(wildcard artifacts/networks/koi-v9-hm10-h256-official.nnue)
FEN ?=
BENCH_DEPTH ?= 10
PGO_BENCH_DEPTH ?= 10
BENCH_HASH ?= 16
BENCH_THREADS ?= 1
PERFT_DEPTH ?= 5
GENERATOR := $(if $(shell command -v ninja 2>/dev/null),-G Ninja,)

ifeq ($(UNAME_S),Darwin)
  CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=Release -DKOI_BUILD_MODULES=OFF -DKOI_STATIC_RUNTIME=OFF
else
  CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=Release -DKOI_BUILD_MODULES=OFF
endif

.PHONY: all release profile-build bench eval compiler perft test docs clean help

all: release

release:
	cmake -S . -B "$(BUILD_DIR)" $(GENERATOR) $(CMAKE_FLAGS)
	cmake --build "$(BUILD_DIR)" --config Release --target koi_engine -j "$(JOBS)"
	@if [ -n "$(EXE)" ]; then cp "$(BUILD_DIR)/koi-engine" "$(EXE)" && chmod +x "$(EXE)"; fi

profile-build:
	rm -rf "$(PGO_GEN_DIR)" "$(PGO_DATA_DIR)"
	mkdir -p "$(PGO_DATA_DIR)"
	@PGO_DATA_ABS=$$(cd "$(PGO_DATA_DIR)" && pwd); \
	cmake -S . -B "$(PGO_GEN_DIR)" $(GENERATOR) $(CMAKE_FLAGS) -DKOI_PGO=generate -DKOI_PGO_PROFILE_FILE="$$PGO_DATA_ABS" && \
	cmake --build "$(PGO_GEN_DIR)" --config Release --target koi_engine -j "$(JOBS)" && \
	if [ -n "$(EVALFILE)" ]; then \
	  LLVM_PROFILE_FILE="$$PGO_DATA_ABS/koi-%p.profraw" "$(PGO_GEN_DIR)/koi-engine" --nnue "$(EVALFILE)" bench "$(PGO_BENCH_DEPTH)" "$(BENCH_HASH)" "$(BENCH_THREADS)"; \
	else \
	  LLVM_PROFILE_FILE="$$PGO_DATA_ABS/koi-%p.profraw" "$(PGO_GEN_DIR)/koi-engine" --hce bench "$(PGO_BENCH_DEPTH)" "$(BENCH_HASH)" "$(BENCH_THREADS)"; \
	fi && \
	COMPILER_ID=$$(cat "$(PGO_GEN_DIR)"/CMakeFiles/*/CMakeCXXCompiler.cmake 2>/dev/null | sed -n 's/^set(CMAKE_CXX_COMPILER_ID "\([^"]*\)").*/\1/p' | head -n 1); \
	if echo "$$COMPILER_ID" | grep -q "Clang"; then \
	  if [ "$(UNAME_S)" = "Darwin" ]; then \
	    xcrun llvm-profdata merge -output="$$PGO_DATA_ABS/koi.profdata" "$$PGO_DATA_ABS"/*.profraw; \
	  else \
	    llvm-profdata merge -output="$$PGO_DATA_ABS/koi.profdata" "$$PGO_DATA_ABS"/*.profraw; \
	  fi; \
	  PGO_ARG="$$PGO_DATA_ABS/koi.profdata"; \
	else \
	  PGO_ARG="$$PGO_DATA_ABS"; \
	fi && \
	rm -rf "$(BUILD_DIR)" && \
	cmake -S . -B "$(BUILD_DIR)" $(GENERATOR) $(CMAKE_FLAGS) -DKOI_PGO=use -DKOI_PGO_PROFILE_FILE="$$PGO_ARG" && \
	cmake --build "$(BUILD_DIR)" --config Release --target koi_engine -j "$(JOBS)" && \
	if [ -n "$(EXE)" ]; then cp "$(BUILD_DIR)/koi-engine" "$(EXE)" && chmod +x "$(EXE)"; fi

bench: release
	@if [ -n "$(EVALFILE)" ]; then \
	  KOI_NNUE_PATH="$(EVALFILE)" "$(BUILD_DIR)/koi-engine" bench "$(BENCH_DEPTH)" "$(BENCH_HASH)" "$(BENCH_THREADS)"; \
	else \
	  "$(BUILD_DIR)/koi-engine" bench "$(BENCH_DEPTH)" "$(BENCH_HASH)" "$(BENCH_THREADS)"; \
	fi

eval: release
	@if [ -n "$(EVALFILE)" ]; then \
	  if [ -n "$(FEN)" ]; then \
	    "$(BUILD_DIR)/koi-engine" --nnue "$(EVALFILE)" eval "$(FEN)"; \
	  else \
	    "$(BUILD_DIR)/koi-engine" --nnue "$(EVALFILE)" eval; \
	  fi; \
	else \
	  if [ -n "$(FEN)" ]; then \
	    "$(BUILD_DIR)/koi-engine" --hce eval "$(FEN)"; \
	  else \
	    "$(BUILD_DIR)/koi-engine" --hce eval; \
	  fi; \
	fi

compiler: release
	"$(BUILD_DIR)/koi-engine" compiler

perft: release
	"$(BUILD_DIR)/koi-engine" perft "$(PERFT_DEPTH)"

test:
	cmake -S . -B "$(BUILD_DIR)" $(GENERATOR) $(CMAKE_FLAGS)
	cmake --build "$(BUILD_DIR)" --config Release -j "$(JOBS)"
	ctest --test-dir "$(BUILD_DIR)" -C Release -j "$(JOBS)" --output-on-failure

docs:
	python3 tools/docs/prepare_pages.py
	mkdocs build --strict

clean:
	rm -rf "$(BUILD_DIR)" "$(PGO_GEN_DIR)" "$(PGO_DATA_DIR)"

help:
	@echo "Koi Makefile targets (Linux x86-64 / macOS arm64):"
	@echo "  make [EXE=<path>]                      Build koi-engine Release (copies binary to EXE for OpenBench)"
	@echo "  make profile-build [EVALFILE=...]      Build PGO-optimized koi-engine Release binary"
	@echo "  make bench [EVALFILE=...]              Build and run deterministic OpenBench benchmark"
	@echo "  make eval [EVALFILE=...] [FEN=...]     Evaluate startpos or FEN with NNUE/HCE"
	@echo "  make compiler                          Build and print compiler / ISA metadata"
	@echo "  make perft [PERFT_DEPTH=5]             Build and run timed startpos perft divide"
	@echo "  make test                              Build full suite and run CTest"
	@echo "  make docs                              Build MkDocs Material site strictly"
	@echo "  make clean                             Remove $(BUILD_DIR) and PGO artifacts"
