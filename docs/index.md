# Koi Chess Engine

**Koi** is an open-source **C++26 UCI chess engine** for standard chess on **Windows x64**, **Linux x86-64**, and **macOS arm64 (Apple Silicon)**.

It combines Fancy Magic / BMI2 PEXT bitboards, iterative-deepening Principal Variation Search (PVS), Lazy SMP parallelism, and a lockless 24-byte XOR transposition table with a **HalfKA-HM SCReLU NNUE** evaluator, a built-in tapered classical evaluator (`EvalMode=hce`), and zero-configuration support for Polyglot opening books and Syzygy endgame tablebases.

<div class="grid cards" markdown>

-   **Download & Play**

    ---

    Grab the latest self-contained Windows, Linux, or macOS archive with bundled `koi.nnue` and verified CC0 `book.bin`.

    [GitHub Releases](https://github.com/nhatquang1701/Koi/releases) · [Changelog](CHANGELOG.md)

-   **User Guide & GUIs**

    ---

    Configure Koi in En Croissant, Pawn Appétit, Cute Chess, Lucas Chess, or Arena, and tune UCI options, books, and Syzygy tablebases.

    [Open User Guide](docs/USER_GUIDE.md)

-   **Architecture & Engine Spec**

    ---

    Explore the module map, concurrency ownership, search pruning formulas, time management math, and NNUE container specs.

    [Architecture](docs/ARCHITECTURE.md) · [Engine Spec](docs/ENGINE_SPEC.md)

-   **NNUE Pipeline & Development**

    ---

    Train HalfKA-HM networks with Bullet or PyTorch, run SPRT strength gates, and contribute with CMake Presets.

    [NNUE Pipeline](docs/NNUE_PIPELINE.md) · [Contributing](CONTRIBUTING.md)

</div>

---

## 60-Second Quick Start

1. **Extract a release archive** (or build via `cmake --preset <platform>-release`).
2. **Register `koi-engine`** (`koi-engine.exe` on Windows) as a UCI engine in your chess GUI.
3. **Play or analyze** with `Hash = 512` and `Threads = 1` (deterministic) or `Threads = 4` (Lazy SMP).

Koi discovers its assets automatically with zero config files:

- **NNUE Network:** Loads `koi.nnue` beside the executable automatically at boot (`--hce`, `EvalMode=hce`, or `KOI_EVALUATOR=hce` switches to the built-in classical evaluator; `--nnue <path>` loads an explicit network).
- **Opening Book:** Ships a compact Stockfish-verified CC0 Polyglot `book.bin` beside the executable (auto-detected unless `OwnBook=false` is explicitly set).
- **Syzygy Tablebases:** Drop 3–7 man `.rtbw`/`.rtbz` files into `syzygy/` beside the executable (or set `SyzygyPath`) for automatic root and interior probing.

---

## Interactive CLI Highlights

```bash
koi-engine bench                  # OpenBench-compatible deterministic benchmark
koi-engine compiler               # Print toolchain, C++ standard, and ISA capabilities
koi-engine d [fen]                # Print ASCII board, FEN, Zobrist key, and move count
koi-engine key [fen]              # Print position, pawn, and Polyglot hash keys
koi-engine moves [fen]            # List sorted legal UCI moves
koi-engine flip [fen]             # Toggle side to move when legal (updated FEN)
koi-engine book [fen]             # Probe active or --book <path> Polyglot weights
koi-engine tb [fen]               # Probe active or --syzygy <dir> Syzygy tablebases
koi-engine see <move> [fen]       # Compute static exchange evaluation (SEE) in cp
koi-engine eval [fen]             # Inspect NNUE metadata or HCE term breakdown table
koi-engine --hce eval [fen]       # Evaluate directly with classical HCE (no network)
koi-engine --nnue <path> bench    # Benchmark an explicit NNUE weight file
koi-engine perft 5 [fen]          # Run timed perft divide (depth 1..10)
koi-engine --version              # Print version and git build commit
koi-engine --engine-json          # Print En Croissant / Pawn Appétit JSON profile
```

Inside a UCI session, Koi also accepts `d`, `fen`, `key`, `moves`, `book`, `tb`, `eval`, `flip`, `see <move>`, `compiler`, `bench [depth [hash [threads]]]`, `perft <depth>`, `wait`, and `help`. For full flag details, see the [CLI & Interactive Reference](docs/CLI.md).

---

## Explore the Documentation

| Section | Contents |
| :--- | :--- |
| **[User Guide](docs/USER_GUIDE.md)** | Installation, build presets, UCI options, time controls, GUI registration, opening books, and Syzygy tablebases. |
| **[CLI & Interactive Reference](docs/CLI.md)** | Zero-network subcommands (`compiler`, `d`, `key`, `moves`, `flip`, `book`, `tb`, `see`, `perft`), `--hce`/`--nnue`/`--book`/`--syzygy` flags, `eval`, interactive commands, protocols, and dev tools. |
| **[Lichess Bot](docs/lichess-bot.md)** | Running Koi on Lichess via `tools/bot/koi-bot.sh` with automated matchmaking and release updates. |
| **[System Architecture](docs/ARCHITECTURE.md)** | Interactive module diagram, translation-unit responsibilities, threading model, and 1.0.0 architecture boundaries. |
| **[Engine Specification](docs/ENGINE_SPEC.md)** | Complete technical specification of rules, search heuristics, NNUE v2–v9 containers, time formulas, and UCI contracts. |
| **[NNUE Pipeline](docs/NNUE_PIPELINE.md)** | Self-play corpus generation, Bullet/PyTorch training, quantization, and equal-time SPRT promotion gates. |
| **[Contributing](CONTRIBUTING.md)** | Prerequisites, CMake Presets, CTest matrix, engineering rules, and pull-request standards. |
| **[Developer Tools](tools/README.md) & [Test Suite](tests/README.md)** | `koi-bench`, `koi-perft`, `koi-replay`, PGO, Elo/SPRT harnesses, reliability fuzzers, and CTest labels. |
| **[Releases](docs/releases/v1.2.0.md) & [Security](SECURITY.md)** | Release highlights, strength measurements, changelog, and vulnerability disclosure policy. |
