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
- **PowerShell (`pwsh`):** Enables the cross-platform UCI and process integration tests.
- **Python 3 (Optional):** Enables the measurement, dataset, and reliability fuzzer suites:
  ```bash
  python -m pip install -r ./tools/measurement/requirements.txt
  ```

---

## Building Koi

### Windows (x64)

```powershell
cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
cmake --build build\release --config Release
```

### Linux (x86-64)

Substitute `clang-18` / `clang++-18` when building with Clang:

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14
cmake --build build/release --config Release
```

### macOS (Apple Silicon `arm64`)

```bash
brew install llvm ninja
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang" \
  -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++" \
  -DKOI_BUILD_MODULES=OFF -DKOI_STATIC_RUNTIME=OFF
cmake --build build/release --config Release
```

For Debug builds, replace `build\release` / `Release` with `build\debug` / `Debug`.

### CPU Dispatch & Executables

The Release build produces `build\release\koi-engine.exe` (`build/release/koi-engine` on Linux and macOS). On Windows and Linux x86-64, `koi-engine` automatically dispatches to `koi-engine-avx512` or `koi-engine-avx2` when the host CPU supports them (and fast `PEXT`), or runs its baseline code in place. Set `KOI_CPU_VARIANT=generic|avx2|avx512` to force a specific binary. The selector is completely silent on `stdout` and `stderr` so UCI GUIs never see non-protocol output.

Quick UCI smoke check:

```powershell
@('uci','isready','position startpos','go depth 2','stop','quit') | & .\build\release\koi-engine.exe
```

---

## Running the Test Suite

```powershell
# Full parallel Release suite
ctest --test-dir build\release -C Release -j 8 --output-on-failure

# 64-position tactical strength gate
ctest --test-dir build\release -C Release -R koi_strength_tests --output-on-failure

# Fast unit subset via the PowerShell runner (captures JUnit XML and LastTest.log)
.\tools\test\run_tests.ps1 -Label unit
```

On Linux and macOS, use `build/release` with forward slashes.

- **Strict assertions:** Never weaken or delete test assertions or `XFAIL` entries to force a green run; an unexpected pass (`XPASS`) fails the suite by design so fixed entries are cleaned up immediately.
- **Determinism contract:** `Threads = 1` is strictly deterministic. Multi-threaded tests (`Threads > 1`) exercise Lazy SMP and must assert invariants (legality, completion, depth, ranked MultiPV) rather than byte-for-byte PV equality.
- **Full release verification gate:** `.\tools\build\release_verify.ps1`.

---

## Submitting Changes

1. **Branch** from `koi-engine`.
2. **Commit** with a clear, imperative summary (e.g., `Fix mate score normalization at root`).
3. **Verify** against the CI matrix when opening a pull request:
   - **Windows (`.github/workflows/windows.yml`):** Full Release CTest, Debug smoke (`-LE heavy`), MSVC AddressSanitizer (`-DKOI_SANITIZE=ON`), shadow-diff oracle (`-DKOI_BUILD_SHADOW_DIFF=ON`), release package verification, and a 3-pass no-retry flake gate.
   - **Linux (`.github/workflows/linux.yml`):** GCC 14 and Clang 18 Release suites, modules-off build, portable Ubuntu 22.04 tarball smoke, no-retry flake gate, and GCC ThreadSanitizer (`linux-tsan`).
   - **macOS (`.github/workflows/macos.yml`):** Apple Silicon Release suite and `macos-arm64` tarball verification.

---

## Engineering Rules

- **Handshake stability:** Keep the UCI handshake output byte-identical unless a change intentionally updates it; update `tests/data/uci/handshake.txt`, tests, and documentation together.
- **Protocol purity:** `stdout` is reserved exclusively for valid UCI output (`UciController` is the sole protocol writer), with exactly one legal `bestmove` emitted per search.
- **Evaluator contract:** NNUE is the default and required boot evaluator; the clean classical evaluator is selectable via `EvalMode=hce` or `KOI_EVALUATOR=hce`.
- **Evidence discipline:** Elo, CPL, and NPS figures are measurement reports—never unverified claims or fragile CI thresholds.
- **Repository hygiene:** Never commit `build/`, generated corpora, trained `.nnue` files, user `book.bin` files, Syzygy tablebases, or temporary scratch directories.
- **License compliance:** Keep all third-party license notices intact. The Stockfish measurement oracle (`third_party/stockfish-19`, GPL-3.0) is for local testing only and must never be packaged or redistributed with Koi.

---

## Documentation References

- **[Repository README](README.md)** — Overview, quick start, and build instructions
- **[User Guide](docs/USER_GUIDE.md)** — UCI options, time controls, GUI registration, and optional assets
- **[System Architecture](docs/ARCHITECTURE.md)** — Module map, concurrency ownership, and 1.0.0 architecture boundaries
- **[Test Suite](tests/README.md)** — Test targets, CTest labels, environment variables, and progress indicators
- **[Developer Tools](tools/README.md)** — Benchmarks, PGO, oracle analysis, and match harnesses
- **[Documentation Map](docs/README.md)** — Full index of project documentation

---

## License

Contributions to Koi are licensed under the [GNU General Public License v3.0](LICENSE), Copyright (c) 2026 Nguyễn Nhật Quang.
