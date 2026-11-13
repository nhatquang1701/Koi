<h1 align="center">Koi</h1>

<p align="center">
  <strong>A fast, modern C++26 UCI chess engine for standard chess.</strong><br>
  <a href="https://nhatquang1701.github.io/Koi/"><strong>Explore the Documentation »</strong></a>
</p>

<p align="center">
  <a href="docs/USER_GUIDE.md#supported-uci-behavior"><img src="https://img.shields.io/badge/PROTOCOL-UCI-2e9d59?style=for-the-badge" alt="Protocol: UCI"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/LICENSE-MIT-2e9d59?style=for-the-badge" alt="License: MIT"></a>
  <img src="https://img.shields.io/badge/LANGUAGE-C%2B%2B26-2e9d59?style=for-the-badge" alt="Language: C++26">
  <img src="https://img.shields.io/badge/PLATFORMS-Windows%20x64%20%7C%20Linux%20x86_64%20%7C%20macOS%20arm64-2e9d59?style=for-the-badge" alt="Platforms: Windows x64, Linux x86-64, and macOS arm64">
</p>

<p align="center">
  <a href="https://github.com/nhatquang1701/Koi/releases">Releases</a>
  ·
  <a href="docs/USER_GUIDE.md">User Guide</a>
  ·
  <a href="docs/ARCHITECTURE.md">Architecture</a>
  ·
  <a href="https://github.com/nhatquang1701/Koi/issues/new">Report an Issue</a>
</p>

---

## Overview

**Koi** is an open-source Universal Chess Interface (UCI) engine written in C++26 for Windows x64, Linux x86-64, and macOS arm64 (Apple Silicon). Designed for responsive GUI analysis and competitive play, Koi pairs an iterative-deepening Principal Variation Search (PVS) with Lazy SMP parallelism, a persistent 24-byte transposition table, and a bundled HalfKA NNUE evaluator.

| Area | Capabilities |
| :--- | :--- |
| **Search** | Iterative-deepening alpha-beta (PVS) · Lazy SMP (`Threads > 1`) · Root-parallel `MultiPV` (up to 16 lines) · Deterministic single-thread mode (`Threads = 1`) |
| **Evaluation** | Bundled HalfKA NNUE network (`nnue/koi.nnue`, auto-discovered at boot) · Clean tapered classical evaluator selectable via `EvalMode=hce` or `KOI_EVALUATOR=hce` |
| **Protocol** | Standard FIDE chess over UCI · Flag-proof time management with adaptive network-overhead learning · In-place `ponder` / `ponderhit` · `UCI_ShowWDL` |
| **Assets** | Bundled CC0 Polyglot opening book (`book/koi.bin`) · Automatic detection of local books (`book.bin`) and Syzygy tablebases (`syzygy/`) |
| **Hardware** | Automatic CPU dispatch on x86-64 (`koi-engine-avx512` → `koi-engine-avx2` → baseline) · Native Apple Silicon (`arm64`) support |

---

## Quick Start

### 1. Download a Release Package

Prebuilt archives and `.sha256` checksum sidecars are published on [GitHub Releases](https://github.com/nhatquang1701/Koi/releases):

- **Windows x64:** `koi-engine-v1.1.6.zip`
- **Linux x86-64:** `koi-engine-v1.1.6-linux-x86_64.tar.gz`
- **macOS arm64 (Apple Silicon):** `koi-engine-v1.1.6-macos-arm64.tar.gz`

Each release archive is self-contained and includes the engine binaries, the gated network (`nnue/koi.nnue`), the custom CC0 opening book (`book/koi.bin`), required third-party notices, and a cryptographic `package.json` provenance manifest.

### 2. Register in Your Chess GUI

1. Extract the archive to a permanent folder.
2. Add `koi-engine.exe` (Windows) or `koi-engine` (Linux / macOS) as a UCI engine in **En Croissant**, **Lucas Chess**, **Arena**, **Cute Chess**, or any compatible UCI interface.
3. Recommended starting options:
   - `Hash = 512` (shared transposition table in MB)
   - `Threads = 1` for deterministic single-thread search, or `Threads = 4` for multi-threaded Lazy SMP

Koi works out of the box with zero configuration files:

- **NNUE network:** Automatically discovered from `nnue/koi.nnue` or `koi.nnue` beside the executable.
- **Opening book:** Copy `book/koi.bin` beside the binary as `book.bin` to enable automatic Polyglot book lookup (or point `BookFile` at any `.bin` book and set `OwnBook = true`).
- **Syzygy tablebases:** Place `.rtbw` and `.rtbz` files in a `syzygy/` folder beside the executable (or set `SyzygyPath`) for automatic root and interior probing.

---

## Building from Source

Koi requires **CMake 3.31+**, **Ninja**, and a C++26 compiler (**MSVC 19.40+** on Windows, **GCC 14+** or **Clang 18+** on Linux, or **Homebrew LLVM Clang 19+ / AppleClang 18+** on macOS arm64).

**Windows (x64 Visual Studio Developer PowerShell):**

```powershell
cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
cmake --build build\release --config Release
ctest --test-dir build\release -C Release -j 8 --output-on-failure
```

**Linux (x86-64):**

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14
cmake --build build/release --config Release
ctest --test-dir build/release -C Release -j 8 --output-on-failure
```

**macOS (Apple Silicon `arm64`):**

```bash
brew install llvm ninja
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang" \
  -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++" \
  -DKOI_BUILD_MODULES=OFF -DKOI_STATIC_RUNTIME=OFF
cmake --build build/release --config Release
ctest --test-dir build/release -C Release -j 8 --output-on-failure
```

See the [User Guide](docs/USER_GUIDE.md#build-prerequisites) for complete build options, CPU variant overrides (`KOI_CPU_VARIANT`), and developer tools (`koi-bench`, `koi-perft`, `koi-replay`).

---

## Documentation

- **[Documentation Site](https://nhatquang1701.github.io/Koi/)** — Full browsable documentation with search and interactive diagrams.
- **[User Guide](docs/USER_GUIDE.md)** — Installation, UCI options reference, time controls, GUI workflows, and optional assets.
- **[System Architecture](docs/ARCHITECTURE.md)** — Module boundaries, search/evaluation data flow, concurrency model, and fail-closed contracts.
- **[Engine Specification](docs/ENGINE_SPEC.md)** — Complete technical specification of rules, search heuristics, NNUE containers, and UCI semantics.
- **[NNUE Pipeline](docs/NNUE_PIPELINE.md)** — Self-play data generation, PyTorch/Bullet training, quantization, and SPRT promotion gates.
- **[Developer Tools](tools/README.md) & [Test Suite](tests/README.md)** — Benchmarks, Elo measurement harnesses, fuzzers, and CTest layout.
- **[Documentation Map](docs/README.md)** — Index of all guides, release notes, and historical records.

> **Note on Naming:** The engine is called **Koi** (reporting `id name Koi <version>` in the UCI handshake). Releases through v1.1.5 used "Koi Engine" in prose; executable and archive file names retain `koi-engine` for script and GUI compatibility.

---

## License

Koi is released under the [MIT License](LICENSE). Release packages also include the license notices for vendored components (`Disservin/chess-library` and `Fathom`).
