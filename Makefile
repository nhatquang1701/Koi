# OpenBench-compatible convenience Makefile for POSIX hosts (Linux x86-64, macOS arm64).
# Windows x64 builds use CMake + Ninja directly (`cmake --preset windows-release`).

UNAME_S := $(shell uname -s)
BUILD_DIR ?= build/make-release
ASAN_DIR := $(BUILD_DIR)-asan
PGO_GEN_DIR := $(BUILD_DIR)-pgo-gen
PGO_DATA_DIR := $(BUILD_DIR)-pgo-data
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
EXE ?=
EVALFILE ?= $(wildcard artifacts/networks/koi-v9-hm10-h256-official.nnue)
FEN ?=
HASH ?=
THREADS ?=
BENCH_DEPTH ?= 10
PGO_BENCH_DEPTH ?= 10
BENCH_HASH ?= 16
BENCH_THREADS ?= 1
SPEEDTEST_THREADS ?= 1
SPEEDTEST_HASH ?= 128
SPEEDTEST_SECONDS ?= 3
PERFT_DEPTH ?= 5
GENERATOR := $(if $(shell command -v ninja 2>/dev/null),-G Ninja,)

ifeq ($(UNAME_S),Darwin)
  CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=Release -DKOI_BUILD_MODULES=OFF -DKOI_STATIC_RUNTIME=OFF
else
  CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=Release -DKOI_BUILD_MODULES=OFF
endif

.PHONY: all release tools profile-build bench speedtest eval nnue options engine-json version compiler perft d smoke test asan docs clean help

all: release

release:
	cmake -S . -B "$(BUILD_DIR)" $(GENERATOR) $(CMAKE_FLAGS)
	cmake --build "$(BUILD_DIR)" --config Release --target koi_engine -j "$(JOBS)"
	@if [ -n "$(EXE)" ]; then cp "$(BUILD_DIR)/koi-engine" "$(EXE)" && chmod +x "$(EXE)"; fi

tools:
	cmake -S . -B "$(BUILD_DIR)" $(GENERATOR) $(CMAKE_FLAGS)
	cmake --build "$(BUILD_DIR)" --config Release --target koi_engine koi_bench koi_perft koi_replay koi_eval_features koi_nnue_fixture -j "$(JOBS)"

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
	  "$(BUILD_DIR)/koi-engine" --nnue "$(EVALFILE)" bench "$(BENCH_DEPTH)" "$(BENCH_HASH)" "$(BENCH_THREADS)"; \
	else \
	  "$(BUILD_DIR)/koi-engine" --hce bench "$(BENCH_DEPTH)" "$(BENCH_HASH)" "$(BENCH_THREADS)"; \
	fi

nnue: release
	@if [ -n "$(EVALFILE)" ]; then \
	  "$(BUILD_DIR)/koi-engine" nnue "$(EVALFILE)"; \
	else \
	  "$(BUILD_DIR)/koi-engine" nnue; \
	fi

options: release
	@if [ -n "$(EVALFILE)" ]; then \
	  "$(BUILD_DIR)/koi-engine" --nnue "$(EVALFILE)" options; \
	else \
	  "$(BUILD_DIR)/koi-engine" --hce options; \
	fi

speedtest: release
	@if [ -n "$(EVALFILE)" ]; then \
	  "$(BUILD_DIR)/koi-engine" --nnue "$(EVALFILE)" speedtest "$(SPEEDTEST_THREADS)" "$(SPEEDTEST_HASH)" "$(SPEEDTEST_SECONDS)"; \
	else \
	  "$(BUILD_DIR)/koi-engine" --hce speedtest "$(SPEEDTEST_THREADS)" "$(SPEEDTEST_HASH)" "$(SPEEDTEST_SECONDS)"; \
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

version: release
	"$(BUILD_DIR)/koi-engine" version

engine-json: release
	@ARGS="--engine-json"; \
	if [ -n "$(THREADS)" ]; then ARGS="--threads $(THREADS) $$ARGS"; fi; \
	if [ -n "$(HASH)" ]; then ARGS="--hash $(HASH) $$ARGS"; fi; \
	"$(BUILD_DIR)/koi-engine" $$ARGS

compiler: release
	"$(BUILD_DIR)/koi-engine" compiler

perft: release
	@if [ -n "$(FEN)" ]; then \
	  "$(BUILD_DIR)/koi-engine" perft "$(PERFT_DEPTH)" "$(FEN)"; \
	else \
	  "$(BUILD_DIR)/koi-engine" perft "$(PERFT_DEPTH)"; \
	fi

d: release
	@if [ -n "$(FEN)" ]; then \
	  "$(BUILD_DIR)/koi-engine" d "$(FEN)"; \
	else \
	  "$(BUILD_DIR)/koi-engine" d; \
	fi

smoke: release
	@if [ -n "$(EVALFILE)" ]; then \
	  "$(BUILD_DIR)/koi-engine" --nnue "$(EVALFILE)" -c "uci" -c "isready" -c "position startpos" -c "go depth 2"; \
	else \
	  "$(BUILD_DIR)/koi-engine" --hce -c "uci" -c "isready" -c "position startpos" -c "go depth 2"; \
	fi

test:
	cmake -S . -B "$(BUILD_DIR)" $(GENERATOR) $(CMAKE_FLAGS)
	cmake --build "$(BUILD_DIR)" --config Release -j "$(JOBS)"
	ctest --test-dir "$(BUILD_DIR)" -C Release -j "$(JOBS)" --output-on-failure

asan:
	cmake -S . -B "$(ASAN_DIR)" $(GENERATOR) -DCMAKE_BUILD_TYPE=Debug -DKOI_SANITIZE=ON -DKOI_BUILD_MODULES=OFF -DKOI_STATIC_RUNTIME=OFF
	cmake --build "$(ASAN_DIR)" --config Debug -j "$(JOBS)"
	ctest --test-dir "$(ASAN_DIR)" -C Debug -L unit -LE heavy -j "$(JOBS)" --output-on-failure

docs:
	python3 tools/docs/prepare_pages.py
	mkdocs build --strict

clean:
	rm -rf "$(BUILD_DIR)" "$(ASAN_DIR)" "$(PGO_GEN_DIR)" "$(PGO_DATA_DIR)"

help:
	@echo "Koi Makefile targets (Linux x86-64 / macOS arm64):"
	@echo "  make [EXE=<path>]                      Build koi-engine Release (copies binary to EXE for OpenBench)"
	@echo "  make tools                             Build koi-engine and all companion CLI developer tools"
	@echo "  make profile-build [EVALFILE=...]      Build PGO-optimized koi-engine Release binary"
	@echo "  make bench [EVALFILE=...]              Build and run deterministic OpenBench benchmark"
	@echo "  make speedtest [SPEEDTEST_THREADS=1]   Build and run timed NPS throughput benchmark"
	@echo "  make eval [EVALFILE=...] [FEN=...]     Evaluate startpos or FEN with NNUE/HCE"
	@echo "  make nnue [EVALFILE=...]               Inspect NNUE container metadata and payload SHA-256"
	@echo "  make options [EVALFILE=...]            Print active engine configuration & UCI options"
	@echo "  make engine-json [THREADS=N] [HASH=MB] Print En Croissant / Pawn Appetit JSON profile"
	@echo "  make version                           Build and print engine version banner"
	@echo "  make compiler                          Build and print compiler / ISA metadata"
	@echo "  make perft [PERFT_DEPTH=5] [FEN=...]   Build and run timed perft divide"
	@echo "  make d [FEN=...]                       Build and print ASCII board, FEN, key, and FIDE state"
	@echo "  make smoke [EVALFILE=...]              Run one-shot UCI handshake + depth-2 search smoke check"
	@echo "  make test                              Build full suite and run CTest"
	@echo "  make asan                              Build Debug ASan+UBSan and run fast unit tests"
	@echo "  make docs                              Build MkDocs Material site strictly"
	@echo "  make clean                             Remove $(BUILD_DIR), ASan, and PGO artifacts"
