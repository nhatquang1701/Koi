# Contributing to Koi

**Koi** is a C++26 UCI chess engine for Windows x64, Linux x86-64, and macOS arm64 (Apple Silicon), built with MSVC, GCC 14+, or Clang 18+ / AppleClang 18+ using CMake and Ninja. Contributions that preserve protocol purity, determinism, and verified playing strength are welcome.

---

## Prerequisites

- **Compiler & Platform:**
  - **Windows x64:** Visual Studio 2022+ MSVC x64 toolchain (`/std:c++latest`). Run all commands from an **x64 Native Tools Command Prompt** or **x64 Developer PowerShell** (MinGW/GCC/Clang on Windows are not supported).
  - **Linux x86-64:** GCC 14+ or Clang 18+ (`-std=c++26` / `-std=c++2c`) and a `pthreads` libc.
  - **macOS arm64 (Apple Silicon):** Homebrew LLVM Clang 19+ (`brew install llvm ninja`) or AppleClang 18+, configured with `-DKOI_BUILD_MODULES=OFF -DKOI_STATIC_RUNTIME=OFF`.
- **CMake 3.31+:** On distributions that ship an older CMake (such as Ubuntu 24.04), upgrade via `python3 -m pip install --upgrade cmake` or the Kitware APT repository.
- **Ninja:** Required for fast builds and C++26 named-module compilation.
- **PowerShell (`pwsh`):** Enables cross-platform UCI, packaging, and architecture boundary tests.
- **Python 3 (Optional):** Enables measurement, dataset, docs (`mkdocs`), and reliability fuzzer suites:
  ```bash
  python -m pip install -r ./tools/measurement/requirements.txt
  ```

---

## Building Koi

Use the checked-in [CMakePresets.json](CMakePresets.json) presets for standard builds:

```bash
# Windows x64 (x64 Visual Studio Developer PowerShell)
cmake --preset windows-release && cmake --build --preset windows-release

# Linux x86-64 (GCC 14; or linux-clang-release)
cmake --preset linux-gcc-release && cmake --build --preset linux-gcc-release

# macOS arm64 (Apple Silicon)
cmake --preset macos-arm64-release && cmake --build --preset macos-arm64-release
```

Available presets: `windows-release`, `windows-debug`, `windows-asan`, `linux-gcc-release`, `linux-clang-release`, `linux-clang-asan`, `linux-gcc-tsan`, `macos-arm64-release`, `macos-arm64-debug`, and `macos-arm64-asan`. Equivalent explicit `cmake -S . -B build/release -G Ninja ...` commands remain documented in the [User Guide](docs/USER_GUIDE.md#build-prerequisites).

### CPU Dispatch & Quick Smoke Check

On Windows and Linux x86-64, `koi` automatically dispatches to `koi-avx512` or `koi-avx2` when the host CPU supports them (with fast `PEXT`), or runs the baseline build in place. Set `KOI_CPU_VARIANT=generic|avx2|avx512` to force a specific binary.

```bash
# Deterministic OpenBench-compatible node benchmark & timed NPS throughput check (plus Makefile targets: `make tools`, `make smoke`, `make speedtest`, `make profile-build`, `make asan`, `make engine-json`, `make nnue`, `make options`, `make eval`, `make d`, `make epd`, `make history`, `make pgn`, `make version`, `make compiler`, `make perft`, `make perft-suite`)
./build/windows-release/koi.exe bench
./build/windows-release/koi.exe speedtest 1 128 3

# Zero-network position & asset inspection, compiler/ISA readout, eval (--hce / --nnue <path>), & fast perft
./build/windows-release/koi.exe compiler
./build/windows-release/koi.exe --hce eval
./build/windows-release/koi.exe d
./build/windows-release/koi.exe see e2e4
./build/windows-release/koi.exe perft 4 "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"

# Inline UCI smoke check with explicit hash/threads (or `printf "...\nwait\nquit\n" | ./build/.../koi`)
./build/linux-gcc-release/koi --hash 64 --threads 1 -c "uci" -c "isready" -c "position startpos" -c "go depth 4"
```

Supported zero-network CLI inspection subcommands also include `version`, `help`, `nnue [path]`, `fen [pos]`, `epd [pos]`, `key [pos]`, `moves [--san] [pos]`, `history [pos]`, `pgn [pos]`, `flip [pos]`, `book [pos]` (optional `--book <path>`), `tb [pos]` (optional `--syzygy <path>`), and `perft --suite`, while evaluator-backed `eval [pos]` and `options` boot the active NNUE network or `--hce`. `[pos]` accepts `startpos`, a quoted/unquoted 4- or 6-field FEN, or `[startpos|fen] [moves <uci|san>...]`.

---

## Running Tests & Verification

```bash
# Full parallel Release test suite (via preset or explicit build dir)
ctest --preset windows-release -j 8 --output-on-failure

# Fast unit subset (with JUnit XML and LastTest.log capture)
pwsh -NoProfile -File tools/test/run_tests.ps1 -Label unit

# Architecture boundary and CI configuration checks
pwsh -NoProfile -File tests/integration/packaging/architecture_boundaries_test.ps1
pwsh -NoProfile -File tests/integration/packaging/ci_configuration_test.ps1

# Documentation strict build (stages from Git's index; run `git add` on new files first)
python tools/docs/prepare_pages.py && mkdocs build --strict
```

- **Strict assertions:** Never weaken or delete test assertions or `XFAIL` entries to force a green run; an unexpected pass (`XPASS`) fails the suite by design.
- **Determinism contract:** `Threads = 1` is strictly deterministic. Multi-threaded tests (`Threads > 1`) validate invariants (legality, completion, depth, ranked MultiPV) rather than byte-for-byte PV equality.
- **Non-functional changes:** Verify that `koi bench` produces an identical node count before and after your change.
- **Functional search/eval changes:** Gate strength changes with equal-time paired matches / SPRT (see [Apple Silicon Worker](docs/mac-worker.md) and [Developer Tools](tools/README.md)).

---

## Submitting Pull Requests

1. **Branch** from `main`.
2. **Format** C++ changes consistently with `.clang-format` and `.editorconfig`.
3. **Commit** with a clear, imperative summary (e.g., `Fix mate score normalization at root`).
4. **Verify** the CI matrix checks:
   - **Windows (`.github/workflows/windows.yml`):** Full Release CTest, Debug smoke (`-LE heavy`), MSVC AddressSanitizer (`-DKOI_SANITIZE=ON`), shadow-diff oracle (`-DKOI_BUILD_SHADOW_DIFF=ON`), package verification, and a 3-pass no-retry flake gate.
   - **Linux (`.github/workflows/linux.yml`):** GCC 14 and Clang 18 Release suites, modules-off build, Ubuntu 22.04 portable tarball smoke, no-retry flake gate, and GCC ThreadSanitizer (`linux-tsan`).
   - **macOS (`.github/workflows/macos.yml`):** Apple Silicon Release suite and `macos-arm64` tarball verification.
   - **Docs (`.github/workflows/docs.yml`):** `prepare_pages.py` + `mkdocs build --strict`.

---

## Engineering Rules

- **Handshake stability:** Keep the UCI handshake output byte-identical unless a change intentionally updates it; update `tests/data/uci/handshake.txt`, tests, and documentation together.
- **Protocol purity:** `stdout` is reserved exclusively for valid protocol output (`UciController` is the sole protocol writer), with exactly one legal `bestmove` emitted per search.
- **Evaluator contract:** NNUE is the default and required boot evaluator; the clean classical evaluator is selectable via `EvalMode=hce` or `KOI_EVALUATOR=hce`.
- **Evidence discipline:** Elo, CPL, and NPS figures are measurement reports—never unverified claims or fragile CI thresholds.
- **Repository hygiene:** Never commit `build/`, generated corpora, trained `.nnue` files, user `book.bin` files, Syzygy tablebases, or temporary scratch directories.
- **License compliance:** Keep all third-party license notices intact. The Stockfish measurement oracle (`third_party/stockfish-19`, GPL-3.0) is for local testing only and must never be packaged or redistributed with Koi.

---

## License

Contributions to Koi are licensed under the [GNU General Public License v3.0](LICENSE), Copyright © 2026 Nguyễn Nhật Quang.
