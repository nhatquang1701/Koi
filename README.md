<h1 align="center">Koi</h1>

<p align="center">
  <strong>A fast, modern C++26 NNUE chess engine for standard chess.</strong><br>
  <a href="https://nhatquang1701.github.io/Koi/"><strong>Documentation</strong></a>
  ·
  <a href="https://github.com/nhatquang1701/Koi/releases"><strong>Releases</strong></a>
  ·
  <a href="CHANGELOG.md"><strong>Changelog</strong></a>
  ·
  <a href="https://github.com/nhatquang1701/Koi/issues/new/choose"><strong>Report an Issue</strong></a>
</p>

<p align="center">
  <a href="https://github.com/nhatquang1701/Koi/actions/workflows/windows.yml"><img src="https://github.com/nhatquang1701/Koi/actions/workflows/windows.yml/badge.svg?branch=main" alt="Windows CI"></a>
  <a href="https://github.com/nhatquang1701/Koi/actions/workflows/linux.yml"><img src="https://github.com/nhatquang1701/Koi/actions/workflows/linux.yml/badge.svg?branch=main" alt="Linux CI"></a>
  <a href="https://github.com/nhatquang1701/Koi/actions/workflows/macos.yml"><img src="https://github.com/nhatquang1701/Koi/actions/workflows/macos.yml/badge.svg?branch=main" alt="macOS CI"></a>
  <a href="https://github.com/nhatquang1701/Koi/actions/workflows/docs.yml"><img src="https://github.com/nhatquang1701/Koi/actions/workflows/docs.yml/badge.svg?branch=main" alt="Docs"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPLv3-0f766e" alt="License: GPLv3"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-26-0f766e" alt="C++26">
</p>

---

## Overview

**Koi** is an open-source, cross-platform UCI chess engine written in C++26 for **Windows x64**, **Linux x86-64**, and **macOS arm64 (Apple Silicon)**. It pairs Fancy Magic / BMI2 PEXT bitboards and an iterative-deepening Principal Variation Search (PVS) with Lazy SMP parallelism, a lockless 24-byte XOR transposition table, and a HalfKA-HM SCReLU NNUE evaluator.

### Playing Strength

- **Koi v1.2.0** measured **2727 Elo** (`95% CI 2674–2778`, `+998 Elo` over v1.1.5) against Stockfish 19 `UCI_Elo` anchors (`mac-mini` arm64, `200 ms`/move, `Threads=4`, `Hash=512`, `no-book`, bundled `v7` h128 net; see [v1.2.0 release notes](docs/releases/v1.2.0.md)).
- **Active development (`main`)** has since promoted **Official NNUE v9** (`halfka-hm-v2`, 10 horizontally-mirrored king buckets, 256×2 SCReLU, 20-slot Finny tables — `+180+ Elo` over `v7`), added PGO+LTO build support, and improved search pruning (`+11 Elo` in `1+0` clock self-play).

---

## Engine Features

| Component | Details |
| :--- | :--- |
| **Board & Movegen** | Fancy Magic Bitboards & x86-64 BMI2 `PEXT` slider tables · O(1) pin-legal move generation · Streamed tactical/check generators · Full FIDE draw detection (threefold, fivefold, 50/75-move, dead material) |
| **Search** | Iterative-deepening PVS · Dynamic aspiration windows · Lockless 24-byte XOR transposition table (`1–4096 MB`) · Lazy SMP (`Threads > 1`) & root-parallel `MultiPV` (up to 16 lines) · Deterministic `Threads = 1` mode |
| **Pruning & Ordering** | Adaptive Null-Move Pruning with verification · Reverse Futility, Futility, Razoring, ProbCut, SEE pruning · Internal Iterative Reductions (IIR) & Singular Extensions / Multi-Cut · Late Move Reductions (LMR) · Quiet, Capture, Pawn, Continuation (6-ply), Counter-Move, and 6-table Static Eval Correction Histories |
| **Evaluation** | **HalfKA-HM v9 NNUE** (`7680 → 256×2 → 1`, SCReLU, 10 mirrored king buckets, 8 output buckets, 20-slot Finny cache) with AVX-512 / AVX2 / ARM NEON kernels · Built-in tapered classical evaluator (`EvalMode=hce` or `KOI_EVALUATOR=hce`) |
| **Protocols & Assets** | **UCI**, **XBoard/CECP v2**, **USI**, **UCCI**, **UCI_Cyclone** (`--protocol` / `Protocol` option) · Flag-proof time management · In-place `ponder`/`ponderhit` · `UCI_ShowWDL` · Bundled Stockfish-verified CC0 Polyglot book (`book.bin`) · Automatic Syzygy 3–7 man WDL/DTZ probing (`syzygy/`) |
| **Hardware & CLI** | Silent x86-64 CPU dispatch (`koi-avx512` → `koi-avx2` → baseline) · Native Apple Silicon `arm64` · Interactive & CLI `bench`, `speedtest`, `options`, `d`, `fen`, `epd`, `key`, `moves`, `pgn`, `book`, `tb`, `eval`, `nnue`, `flip`, `makemove`, `undo`, `history`, `see`, `compiler`, `perft`, and `-c`/`--cmd` batch execution |

---

## Quick Start

### 1. Download a Release

Prebuilt archives and standalone official networks are published on [GitHub Releases](https://github.com/nhatquang1701/Koi/releases) (and standalone networks on the [Koi NNUEs](https://github.com/nhatquang1701/Koi/releases/tag/koi-nnues) pre-release):

| Platform | Archive |
| :--- | :--- |
| **Windows x64** | `koi-v1.2.0.zip` |
| **Linux x86-64** | `koi-v1.2.0-linux-x86_64.tar.gz` |
| **macOS arm64 (Apple Silicon)** | `koi-v1.2.0-macos-arm64.tar.gz` |

Each archive is minimal and self-contained: engine binaries (`koi`, plus AVX2/AVX-512 siblings on x86-64), the promoted NNUE network (`koi.nnue`), the Stockfish-verified CC0 opening book (`book.bin`), and `LICENSE`.

### 2. Add to Your Chess GUI

1. Extract the archive to a permanent folder.
2. Register `koi.exe` (Windows) or `koi` (Linux / macOS) as a UCI engine in **En Croissant**, **Pawn Appétit**, **Cute Chess**, **BanksiaGUI**, **Lucas Chess**, **Arena**, or any standard UCI/XBoard interface.
   - For **Pawn Appétit / En Croissant**, run `koi --engine-json` to print a ready-to-paste JSON profile with a separate `version` badge.
3. Recommended starting options: `Hash = 512`, `Threads = 1` (deterministic) or `Threads = 4` (Lazy SMP).

Koi requires zero configuration files: `koi.nnue`, `book.bin`, and any user-supplied `syzygy/` tablebases beside the executable are discovered automatically at boot.

---

## CLI & Interactive Commands

In addition to standard UCI and XBoard/CECP v2 commands, Koi supports standard chess-engine terminal and benchmark commands:

```bash
# OpenBench-compatible deterministic benchmark (defaults: depth 10, 16 MB hash, 1 thread) and timed speedtest
koi bench [depth [hash [threads]]]
koi speedtest [threads [hash [seconds]]]

# One-shot diagnostics: compiler/ISA, board/keys/moves/flip/book/TB/SEE (zero-network), eval, and timed perft divide
koi compiler
koi nnue [path]                 # verify container metadata & payload SHA-256
koi d [fen]                     # also accepts [startpos|fen] [moves <uci|san>...] (e.g. d moves e4 e5 Nf3)
koi fen [fen]                   # print six-field FEN after optional move sequence
koi epd [fen]                   # print standard EPD (hmvc/fmvn) for startpos, FEN, or moves sequence
koi key [fen]
koi moves [--san] [fen]         # sorted legal moves in UCI or FIDE SAN notation
koi history [fen]               # print origin, plies, UCI moves, SAN movetext, and position command
koi pgn [fen]                   # export position/moves as FIDE PGN
koi flip [fen]
koi book [fen]                  # optional --book <path>
koi tb [fen]                    # optional --syzygy <dir>
koi see <move> [fen]
koi eval [fen]
koi options                     # print live active protocol, evaluator, book, and Syzygy options
koi perft 5 [fen]               # or `koi perft --suite` (6 CPW positions)

# Evaluator selection, initial hash/threads, batch commands, version, GUI JSON, protocol, and help
koi --hce eval [fen]
koi --nnue <path> eval [fen]
koi --hash 64 --threads 4 -c "position startpos moves e2e4" -c "go depth 6"
koi version                       # or -v, --version
koi --engine-json
koi --protocol xboard
koi help                          # or -h, --help
```

Inside an interactive session (`koi`):

| Command | Description |
| :--- | :--- |
| `bench [depth [hash [threads]]]` | Runs the deterministic benchmark suite and prints `Total time (ms)`, `Nodes searched`, and `Nodes/second` |
| `speedtest [threads [hash [seconds]]]` | Runs a timed multi-position NPS throughput benchmark (`defaults: 1 128 3`) |
| `options` | Prints live active UCI option, book, and Syzygy configuration values |
| `d` | Displays an ASCII board of the current position, FEN, Zobrist key, checkers, legal move count, and FIDE game state |
| `fen` | Prints the six-field FEN of the current position |
| `epd` | Prints standard EPD (`hmvc`/`fmvn`) of the current position |
| `key` | Prints the hexadecimal Zobrist position, pawn, and Polyglot book keys |
| `moves [san]` | Lists all sorted legal moves in UCI coordinate notation (or FIDE SAN with `san`) |
| `book` | Probes active Polyglot opening book weights and percentages for the position |
| `tb` | Probes active Syzygy tablebases for root WDL outcome and best moves |
| `eval` | Prints active NNUE/HCE metadata, static evaluation (side-to-move & White cp), heuristic WDL triplet, and game phase |
| `nnue` | Prints active loaded NNUE container metadata and payload SHA-256 checksum (or inactive HCE status) |
| `flip` | Flips the side to move (when legal) for static evaluation comparison |
| `makemove <move...>` | Plays one or more legal UCI or FIDE SAN moves transactionally on the current board |
| `undo [plies]` | Steps back `N` plies (default `1`) in the current position's move history |
| `history` | Prints the position origin (`startpos` or FEN), ply count, UCI moves, FIDE SAN movetext, and full `position ...` command |
| `pgn` | Exports the current position and move history as FIDE PGN (Seven Tag Roster, SAN movetext, result) |
| `annotate <move|game>` | Classifies a UCI/SAN move or current game history with accuracy %, centipawn loss, and NAG glyphs |
| `see <move>` | Computes the bitboard static exchange evaluation (SEE) of a legal UCI or FIDE SAN move in centipawns |
| `compiler` | Prints version, git commit, compiler toolchain, C++ standard, and compiled ISA (`AVX-512` / `AVX2` / `NEON` / `PEXT`) |
| `perft <depth>` / `go perft <depth>` | Runs an interruptible perft divide (`depth 1..10`) from the current position |
| `wait` | Blocks until any finite active search or `perft` finishes (useful in piped scripts) |
| `help` | Displays a summary of all interactive commands |

---

## Building from Source

Requires **CMake 3.31+**, **Ninja**, and a C++26 compiler (**MSVC 19.40+** on Windows x64, **GCC 14+** or **Clang 18+** on Linux x86-64, or **Homebrew LLVM Clang 19+ / AppleClang 18+** on macOS arm64).

### Using CMake Presets

```bash
# Windows x64 (from an x64 Visual Studio Developer PowerShell)
cmake --preset windows-release && cmake --build --preset windows-release && ctest --preset windows-release

# Linux x86-64 (GCC 14; or use linux-clang-release)
cmake --preset linux-gcc-release && cmake --build --preset linux-gcc-release && ctest --preset linux-gcc-release

# macOS arm64 (Apple Silicon)
cmake --preset macos-arm64-release && cmake --build --preset macos-arm64-release && ctest --preset macos-arm64-release
```

Available presets in [CMakePresets.json](CMakePresets.json): `windows-release`, `windows-debug`, `windows-asan`, `linux-gcc-release`, `linux-clang-release`, `linux-clang-asan`, `linux-gcc-tsan`, `macos-arm64-release`, `macos-arm64-debug`, and `macos-arm64-asan`. On Linux and macOS you can also use the OpenBench-compatible [Makefile](Makefile) (`make`, `make tools`, `make profile-build` for PGO, `make asan`, `make EXE=koi`, `make engine-json`, `make smoke`, `make speedtest`, `make bench EVALFILE=...`, `make nnue`, `make options`, `make eval`, `make d`, `make version`, `make compiler`, `make perft PERFT_DEPTH=... [FEN=...]`, `make epd [FEN=...]`, `make history [FEN=...]`, `make pgn [FEN=...]`, `make test`, `make docs`). See the [User Guide](docs/USER_GUIDE.md#build-prerequisites) for Profile-Guided Optimization (`-DKOI_PGO=generate|use`), CPU variant selection (`KOI_CPU_VARIANT`), and developer tools (`koi-bench`, `koi-perft`, `koi-replay`).

---

## Documentation

- **[Documentation Site](https://nhatquang1701.github.io/Koi/)** — Searchable Material site with interactive diagrams.
- **[User Guide](docs/USER_GUIDE.md)** — Installation, UCI options, time controls, GUI setup, books, and Syzygy tablebases.
- **[CLI Reference](docs/CLI.md)** — One-shot CLI subcommands, interactive UCI commands, companion dev tools, and environment variables.
- **[System Architecture](docs/ARCHITECTURE.md)** — Module map, concurrency model, and the 1.0.0 architecture contract.
- **[Engine Specification](docs/ENGINE_SPEC.md)** — Authoritative technical specification of rules, search, NNUE containers, and UCI semantics.
- **[NNUE Pipeline](docs/NNUE_PIPELINE.md)** — Self-play data generation, Bullet/PyTorch training, quantization, and SPRT promotion gates.
- **[Lichess Bot](docs/lichess-bot.md)** — Running Koi online via `lichess-bot` with automated matchmaking and updates.
- **[Contributing](CONTRIBUTING.md)** · **[Developer Tools](tools/README.md)** · **[Test Suite](tests/README.md)** · **[Security Policy](SECURITY.md)**

---

## Credits & Acknowledgments

- **[Disservin/chess-library](https://github.com/Disservin/chess-library)** (MIT) — Vendored for Polyglot/mirror compatibility and differential validation.
- **[Fathom](https://github.com/jdart1/Fathom)** (MIT) — Syzygy endgame tablebase probing.
- **[Bullet](https://github.com/jw1912/bullet)** — GPU NNUE training framework powering Koi's HalfKA-HM v9 networks.
- **[Chess Programming Wiki](https://www.chessprogramming.org/)** and the open-source chess engine community (**Stockfish**, **Viridithas**, **Obsidian**, **PlentyChess**, **Ethereal**) for open research and evaluation techniques.

---

## License

Koi is free software licensed under the [GNU General Public License v3.0](LICENSE) (Copyright © 2026 Nguyễn Nhật Quang).
