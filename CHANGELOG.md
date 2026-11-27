# Changelog

All notable changes to **Koi** are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and Koi adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Official NNUE v9 (`halfka-hm-v2`, 10-bucket horizontal-mirror 256×2 SCReLU)** with 20-slot Finny king-bucket cache, AVX-512 / AVX2 / NEON fused kernels, and multi-stage Bullet training (`+180+ Elo` over v7 `h128`).
- **Interactive & CLI chess-engine commands:** Repeatable `-c` / `--cmd <command>` inline batch execution (supporting UCI/XBoard/USI/UCCI with automatic `wait` + `quit`), Stockfish-style `speedtest [threads [hash [seconds]]]` timed multi-position NPS benchmark (CLI `--speedtest`, interactive `speedtest`, and `make speedtest`), interactive `options` live-configuration readout (`detail::format_options_summary`), one-shot CLI subcommands `compiler`, `d`/`--display [pos]`, `key [pos]`, `moves [pos]`, `flip [pos]`, `book [pos]`, `tb [pos]`, `see <move> [pos]`, `eval [pos]`, `perft <depth> [pos]` (supporting `[startpos|fen] [moves <uci>...]` via `detail::parse_cli_position_spec`), and `bench, --bench [depth [hash [threads]]]`, flags `--hce`, `--nnue <path>`, `--book <path>`, and `--syzygy <path>`, plus interactive `d` (ASCII board, FEN, Zobrist hash, checkers, legal move count), `fen`, `key` (position, pawn, and Polyglot hash keys), `moves`, `book` (`OpeningBook::probe_all` weights), `tb` (Syzygy root WDL probe), `eval` (active NNUE/HCE architecture readout, payload SHA-256 prefix, active output bucket, quantization parameters, static score, WDL probabilities, game phase, plus Stockfish-style HCE term-by-term MG/EG/Total breakdown via `ClassicalEvaluator::trace`), `flip` (legal side-to-move flip), `see <move>` (bitboard static exchange inspection), `perft <depth>` (interruptible divide with elapsed time and NPS), `wait` (script synchronization across UCI and XBoard), `compiler`, `help`, and `--help` / `-h`, plus `nnue_info.py --verify` payload checksum gating.
- **Upgraded developer CLI tools:** Added `--divide`, optional FEN input, and `--help` / `-h` to `koi-perft`, and added consistent `-h` / `--help` support across `koi-replay`, `koi-bench`, `koi-eval-features`, and `koi-nnue-fixture`.
- **Profile-Guided Optimization (PGO)** & build ergonomics: `-DKOI_PGO=generate|use` across MSVC, GCC, and Clang/AppleClang, `CMakePresets.json` (including `macos-arm64-debug` and `linux-gcc-tsan`), an OpenBench-compatible `Makefile` (`profile-build` PGO, `smoke`, `speedtest`, `eval`, `EVALFILE`, `FEN`, `BENCH_DEPTH`/`HASH`/`THREADS`, `compiler`, `perft`, `docs`), least-privilege `contents: read` + auto-cancel `concurrency` groups on CI CTest workflows, and Node 24 releases (`checkout@v7`, `setup-python@v7`, `upload-artifact@v7`, `download-artifact@v8`).
- **Architecture & CLI regression checks:** Froze `src/cli_support.*` isolation boundaries, `detail/uci_support.*` (including `parse_cli_position_spec`, `format_options_summary`, `format_annotate_*`, `flip_position_fen`, UCCI/USI `go` argument translation, and XBoard option/clock/move parsers), `detail/nnue_io.cpp`, `detail/nnue_manifest.hpp`, `detail/nnue_kernels.hpp`, and `docs/CLI.md` navigation in `architecture_boundaries_test.ps1`, added dedicated `cli_support_tests` and direct `detail/uci_support` unit tests (#79 in `uci_controller_tests`), and added `cli_subcommands_python` (covering `koi-engine`, `koi-perft`, `koi-replay`, `koi-bench`, `koi-eval-features`, and `koi-nnue-fixture`) to CTest.
- **Open-source repository infrastructure:** GitHub issue forms, pull request template, Dependabot config, `SECURITY.md`, `CHANGELOG.md`, `.editorconfig`, and `.clang-format`.

### Changed
- **Internal modularization & build hygiene:** Extracted CLI argument parsing and zero-network subcommands out of `main.cpp` into `src/cli_support.{hpp,cpp}`, split pure protocol parsers (UCI, XBoard `option`/centiseconds/coordinate moves), `go` limit & UCCI/USI argument translation, network overhead estimation, FEN flipping, and interactive/annotation formatters (`d`, `key`, `moves`, `book`, `tb`, `eval`, `flip`, `annotate`, `compiler`, `help`) out of `uci_controller.cpp` into protocol-literal-free `detail/uci_support.*`, split container serialization/SHA-256/discovery out of `nnue.cpp` into `detail/nnue_io.cpp` + `detail/nnue_manifest.hpp`, extracted stateless AVX-512/AVX2/NEON/scalar NNUE inference kernels into `detail/nnue_kernels.hpp`, deduplicated `CMakeLists.txt` test helpers, and eliminated MSVC `D9025` and vendored Fathom Clang deprecation warnings.
- **Documentation & site modernization:** Added a dedicated [CLI & Interactive Reference](docs/CLI.md) page, upgraded MkDocs Material navigation/theme with platform build and UCI smoke-test content tabs plus Material admonitions across `USER_GUIDE.md`, `mac-worker.md`, and `lichess-bot.md`, extracted inline Mermaid CSS/JS into `docs/assets/`, added a complete 27-option UCI quick-reference table in `USER_GUIDE.md`, fixed stale `ponderhit`/WDL/`lichess-bot` facts, synced `README.md`, `ENGINE_SPEC.md`, `ARCHITECTURE.md`, `NNUE_PIPELINE.md`, `tools/README.md` (documenting `tools/docs/`, `tools/engine/`, `annotate_pgn.py`, and `sample_release_openings.py`), and `tests/README.md`, and extended `command_fuzz.py` for HCE `eval` table grammar.
- **Lichess bot (`tools/bot/koi-bot.sh`):** Enabled rated bot-vs-bot matchmaking by default (`KOI_BOT_MATCHMAKING`) and added `reconfigure`.

### Removed
- Retired legacy NNUE v2/Lc0 trainers (`train_nnue_sf.py`, `gen_lc0_labels.py`, `lc0_jsonl_to_corpus.py`, `select_lc0_positions.py`), superseded Polyglot book builders (`build_book.py`, `build_database_book.py`, `build_polyglot_subset.py`), legacy `performance_gate.ps1`, obsolete `prune_generated_trees.ps1`, and the obsolete `removed-nnue-studio.md` tombstone.
- Removed dead search constants (`kChildFutilityEvalAboveAlphaMargin`), unreachable LMR history branches, and unused standard library headers across public, internal, and unit-test C++ translation units (`<stdexcept>` in `transposition_table.cpp`, `<atomic>`, `<mutex>`, `<functional>`, `<thread>`, `<condition_variable>`, `<iostream>`, etc.).

## [1.2.0] - 2026-11-21

Crossed the **2600 Elo** milestone to **2727 Elo** (`+998 Elo` over v1.1.5) against Stockfish 19 `UCI_Elo` anchors. Added Fancy Magic Bitboards, O(1) pin-legal move generation, Dual HalfKA v4 with Finny tables and fused AVX2/NEON inference, lockless transposition tables, multi-protocol support (UCI, XBoard/CECP v2, USI, UCCI, UCI_Cyclone), Stockfish-verified CC0 opening book, and switched license to GPL-3.0-or-later.
Full details: [docs/releases/v1.2.0.md](docs/releases/v1.2.0.md).

## [1.1.6] - 2026-11-05

Post-1.1.5 reliability, architecture, evaluation, and packaging release.
Full details: [docs/releases/v1.1.6.md](docs/releases/v1.1.6.md).

## [1.1.5] - 2026-10-31

First packaged multi-platform release bundling the promoted h128 HalfKA v4 NNUE network (`koi-v7-h128-official-20261025`), full-budget explicit `movetime`, CPUID-cache fix, and x86-64 AVX-512/AVX2 auto-dispatch.
Full details: [docs/releases/v1.1.5.md](docs/releases/v1.1.5.md).

## [1.1.4] - 2026-10-20

Search, evaluation, and protocol reliability updates.
Full details: [docs/releases/v1.1.4.md](docs/releases/v1.1.4.md).

## [1.1.3] - 2026-10-12

Search stability and evaluation refinements.
Full details: [docs/releases/v1.1.3.md](docs/releases/v1.1.3.md).

## [1.1.2] - 2026-10-05

Protocol hardening and time-management updates.
Full details: [docs/releases/v1.1.2.md](docs/releases/v1.1.2.md).

## [1.1.1] - 2026-09-28

Bug fixes and packaging polish.
Full details: [docs/releases/v1.1.1.md](docs/releases/v1.1.1.md).

## [1.1.0] - 2026-09-20

Major search and UCI feature update.
Full details: [docs/releases/v1.1.0.md](docs/releases/v1.1.0.md).

## [1.0.0] - 2026-09-05

Initial public release of Koi.
Full details: [docs/releases/v1.0.0.md](docs/releases/v1.0.0.md).
