# Changelog

All notable changes to **Koi** are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and Koi adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Official NNUE v9 (`halfka-hm-v2`, 10-bucket horizontal-mirror 256×2 SCReLU)** with 20-slot Finny king-bucket cache, AVX-512 / AVX2 / NEON fused kernels, and multi-stage Bullet training (`+180+ Elo` over v7 `h128`).
- **Interactive & CLI chess-engine commands:** One-shot CLI subcommands `compiler`, `eval [fen]`, `perft <depth> [fen]`, and `bench, --bench [depth [hash [threads]]]`, plus interactive `d` (ASCII board, FEN, Zobrist hash, checkers, legal move count), `fen`, `key` (position, pawn, and Polyglot hash keys), `moves`, `book` (`OpeningBook::probe_all` weights), `tb` (Syzygy root WDL probe), `eval` (active NNUE/HCE architecture readout, static score, WDL probabilities, game phase), `flip` (legal side-to-move flip), `see <move>` (bitboard static exchange inspection), `perft <depth>`, `wait` (script synchronization for active search/`perft`), `compiler`, `help`, and `--help` / `-h`.
- **Profile-Guided Optimization (PGO)** & build ergonomics: `-DKOI_PGO=generate|use` across MSVC, GCC, and Clang/AppleClang, `CMakePresets.json`, and an OpenBench-compatible `Makefile` (`EVALFILE`, `BENCH_DEPTH`/`HASH`/`THREADS`, `docs`).
- **Architecture & CLI regression checks:** Froze `detail/uci_support.*`, `detail/nnue_io.cpp`, `detail/nnue_manifest.hpp`, and `detail/nnue_kernels.hpp` in `architecture_boundaries_test.ps1`, and added `cli_subcommands_python` to CTest.
- **Open-source repository infrastructure:** GitHub issue forms, pull request template, Dependabot config, `SECURITY.md`, `CHANGELOG.md`, `.editorconfig`, and `.clang-format`.

### Changed
- **Internal modularization & build hygiene:** Split pure protocol parsers, `go` limit parsing, network overhead estimation, and formatters out of `uci_controller.cpp` into `detail/uci_support.*`, split container serialization/SHA-256/discovery out of `nnue.cpp` into `detail/nnue_io.cpp` + `detail/nnue_manifest.hpp`, extracted stateless AVX-512/AVX2/NEON/scalar NNUE inference kernels into `detail/nnue_kernels.hpp`, deduplicated `CMakeLists.txt` test helpers, and eliminated MSVC `D9025` and vendored Fathom Clang deprecation warnings.
- **Tooling updates (`tools/measurement/nnue_info.py`):** Added full inspection and SHA-256 checksum validation support for NNUE `v4` and `v9` (`halfka-hm-v2`) containers alongside `v5`/`v6`/`v8`.
- **Search improvements:** Allowed Reverse Futility Pruning on quiet transposition-table move nodes (`c19_rfp_qtt`, `+11.01 Elo` at `1+0`), dynamic aspiration window widening, and tactical-only ProbCut move generation.
- **GitHub Releases asset policy:** Release assets publish user-facing platform archives (`.zip` / `.tar.gz`) and `koi.nnue` without `.package.json`, `.metadata.json`, or `.sha256` clutter in the Releases UI.
- **Documentation & site modernization:** Upgraded MkDocs Material navigation/theme, extracted inline Mermaid CSS/JS into `docs/assets/`, synced `README.md`, `USER_GUIDE.md`, `ENGINE_SPEC.md`, `ARCHITECTURE.md`, `NNUE_PIPELINE.md`, `tools/README.md`, and `tests/README.md`.
- **Lichess bot (`tools/bot/koi-bot.sh`):** Enabled rated bot-vs-bot matchmaking by default (`KOI_BOT_MATCHMAKING`) and added `reconfigure`.

### Removed
- Retired legacy NNUE v2/Lc0 trainers (`train_nnue_sf.py`, `gen_lc0_labels.py`, `lc0_jsonl_to_corpus.py`, `select_lc0_positions.py`), superseded Polyglot book builders (`build_book.py`, `build_database_book.py`, `build_polyglot_subset.py`), legacy `performance_gate.ps1`, and the obsolete `removed-nnue-studio.md` tombstone.
- Removed dead search constants (`kChildFutilityEvalAboveAlphaMargin`), unreachable LMR history branches, and unused standard headers.

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
