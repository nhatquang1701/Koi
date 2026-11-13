# Documentation Map

Welcome to the **Koi** documentation index. Start with the **[Documentation Home](index.md)** for a guided overview, or jump directly to a reference below.

## Core Guides

| Document | Description |
| :--- | :--- |
| **[Repository README](../README.md)** | Project overview, quick start, download links, and platform build commands. |
| **[User Guide](USER_GUIDE.md)** | Comprehensive manual covering builds, UCI options, time controls, GUI registration (En Croissant, Lucas Chess), optional assets, and measurement workflows. |
| **[Lichess Bot Guide](lichess-bot.md)** | Installing, running, and auto-updating Koi as an online Lichess bot. |

## Architecture & Engineering

| Document | Description |
| :--- | :--- |
| **[System Architecture](ARCHITECTURE.md)** | Module diagram, file-by-file responsibilities under `src/koi`, concurrency ownership, and the 1.0.0 architecture contract. |
| **[Engine Specification](ENGINE_SPEC.md)** | Deep technical specification of rules, move generation, NNUE containers, search heuristics, time management formulas, and UCI protocol behavior. |
| **[NNUE Pipeline](NNUE_PIPELINE.md)** | Self-play corpus generation, dataset encoding, PyTorch/Bullet training, quantization, and equal-time SPRT promotion gates. |
| **[Apple Silicon Worker](mac-worker.md)** | Setting up a macOS `arm64` worker for speed gates, depth sweeps, and self-play matches. |
| **[Contributing](../CONTRIBUTING.md)** | Build prerequisites, test commands, CI matrix, and contributor guidelines. |
| **[Test Suite](../tests/README.md)** | Unit, integration, process, reliability fuzzer, and Python test inventory, labels, and determinism rules. |
| **[Developer Tools](../tools/README.md)** | CLI benchmarks (`koi-bench`, `koi-perft`, `koi-replay`), Stockfish oracle, Elo estimation, and release packaging scripts. |

## Releases & Archive

- **[Release Notes (v1.1.6)](releases/v1.1.6.md)** — Latest release highlights, search/evaluation improvements, and package provenance (earlier notes: [v1.1.5](releases/v1.1.5.md), [v1.1.4](releases/v1.1.4.md), [v1.1.3](releases/v1.1.3.md), [v1.1.2](releases/v1.1.2.md), [v1.1.1](releases/v1.1.1.md), [v1.1.0](releases/v1.1.0.md), [v1.0.0](releases/v1.0.0.md)).
- **[Removed NNUE Studio](history/removed-nnue-studio.md)** — Historical record of the retired tkinter training GUI and the CLI tools that replaced it.

> **Workspace Layout:** Generated build trees belong under `build/` and generated benchmark, match, or verification reports belong under `artifacts/`. Neither directory is committed to the repository.
