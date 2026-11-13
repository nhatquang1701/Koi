# Koi

**Koi** is a modern, open-source C++26 UCI chess engine for standard chess on **Windows x64**, **Linux x86-64**, and **macOS arm64 (Apple Silicon)**.

It combines an iterative-deepening Principal Variation Search (PVS) and Lazy SMP parallelism with a persistent transposition table, a bundled HalfKA NNUE evaluator, a clean classical evaluator (`EvalMode=hce`), and zero-configuration support for Polyglot opening books and Syzygy endgame tablebases.

---

## Quick Navigation

| Goal | Where to Go |
| :--- | :--- |
| **Download & Play** | Grab the latest Windows, Linux, or macOS archive from **[GitHub Releases](https://github.com/nhatquang1701/Koi/releases)**. |
| **Configure a Chess GUI** | Follow the **[User Guide](USER_GUIDE.md#register-in-en-croissant-primary)** for En Croissant, Lucas Chess, Arena, or Cute Chess. |
| **Understand UCI Options** | Browse the **[Supported UCI Behavior](USER_GUIDE.md#supported-uci-behavior)** and **[Engine Specification](ENGINE_SPEC.md#7-uci-surface)**. |
| **Run a Lichess Bot** | Set up an automated online bot with **[Running Koi as a Lichess Bot](lichess-bot.md)**. |
| **Build & Contribute** | Read **[Contributing](../CONTRIBUTING.md)**, **[System Architecture](ARCHITECTURE.md)**, and **[Test Layout](../tests/README.md)**. |

---

## Getting Started in 60 Seconds

1. **Extract a release package** (or [build from source](USER_GUIDE.md#build-prerequisites)).
2. **Register `koi-engine`** (`koi-engine.exe` on Windows) in your UCI chess GUI.
3. **Start playing or analyzing** with `Hash=512` and `Threads=1` (deterministic) or `Threads=4` (Lazy SMP).

Koi requires no external configuration files:

- **NNUE Evaluator:** Bundled at `nnue/koi.nnue` in release archives and discovered automatically at startup. A clean hand-crafted evaluator is also built in and selectable at any time via `setoption name EvalMode value hce` (or `KOI_EVALUATOR=hce` at boot).
- **Opening Book:** Release archives include a custom CC0 Polyglot book at `book/koi.bin`. Copy it next to the binary as `book.bin` for automatic detection, or configure `OwnBook` and `BookFile` in your GUI.
- **Syzygy Tablebases:** Place licensed `.rtbw` / `.rtbz` files in a `syzygy/` directory beside the executable (or set `SyzygyPath`) for automatic root and interior probing.

---

## Documentation Sections

### Using Koi

- **[User Guide](USER_GUIDE.md)** — Complete guide to building, UCI commands and options, time management, MultiPV analysis, GUI setup, opening books, Syzygy tablebases, and release packaging.
- **[Lichess Bot Guide](lichess-bot.md)** — Turn-key macOS and Linux workflow (`tools/bot/koi-bot.sh`) for running Koi on Lichess with automatic release updates.

### Developing Koi

- **[Contributing](../CONTRIBUTING.md)** — Toolchain prerequisites, build commands, CTest workflows, and repository rules.
- **[System Architecture](ARCHITECTURE.md)** — Interactive module diagram, translation-unit responsibilities, threading ownership, and the 1.0.0 architecture contract.
- **[NNUE Pipeline](NNUE_PIPELINE.md)** — End-to-end training loop: Stockfish self-play generation, sparse binary encoding, PyTorch/Bullet training, quantization, and SPRT promotion gates.
- **[Apple Silicon Worker](mac-worker.md)** — Configuring an Apple Silicon Mac for low-noise speed gates, benchmarks, and SPRT matches.
- **[Test Suite](../tests/README.md)** — C++ harness, shard layout, CTest labels, reliability fuzzers, and determinism guarantees.
- **[Developer Tools](../tools/README.md)** — `koi-bench`, `koi-perft`, `koi-replay`, PGO workflow, Stockfish oracle analysis, and Elo estimation harnesses.

### Reference & Release History

- **[Engine Specification](ENGINE_SPEC.md)** — Authoritative technical reference covering position representation, draw rules, search pruning formulas, time allocation math, and UCI protocol contracts.
- **[Documentation Map](README.md)** — Concise directory of all maintained documentation.
- **[Latest Release Notes (v1.1.6)](releases/v1.1.6.md)** — Highlights, search and evaluation upgrades, strength progression, and archive provenance.
