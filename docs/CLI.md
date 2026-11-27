# CLI & Interactive Reference

In addition to headless GUI communication, `koi-engine` and its companion developer binaries provide terminal commands for inspecting boards, hash keys, opening books, Syzygy tablebases, NNUE evaluations, perft counts, and deterministic benchmarks.

---

## `koi-engine` One-Shot CLI Commands

Run one-shot commands directly from a shell (`koi-engine.exe` on Windows, `koi-engine` on Linux and macOS):

```bash
# OpenBench-compatible deterministic benchmark (defaults: depth 10, 16 MB hash, 1 thread)
koi-engine bench [depth [hash [threads]]]
koi-engine --bench [depth [hash [threads]]]

# Build metadata, C++26 standard, and active SIMD ISA (no NNUE network required)
koi-engine compiler
koi-engine --compiler

# Timed perft divide from startpos or custom FEN (depth 1..10; no NNUE network required)
koi-engine perft <depth> [fen]

# Static NNUE/HCE evaluation readout for startpos or custom FEN
koi-engine eval [fen]
koi-engine --eval [fen]

# Identity, GUI JSON profile, initial protocol dialect, and help
koi-engine --version      # or -v
koi-engine --engine-json
koi-engine --protocol <uci|usi|ucci|ucicyclone|xboard>
koi-engine --help         # or -h
```

!!! note "Zero-network subcommands & unquoted FENs"
    `compiler`, `perft`, `--version`, `--help`, and `--engine-json` execute before NNUE network discovery and work without a `koi.nnue` file. Both quoted (`koi-engine perft 2 "..."`) and unquoted 6-field FEN arguments are supported. Invalid perft depths, malformed FENs, or unknown flags exit cleanly with status `2` and a diagnostic on `stderr`. On Linux and macOS, the root `Makefile` wraps these directly as `make bench`, `make compiler`, and `make perft PERFT_DEPTH=5`.

### Example Outputs

=== "`koi-engine bench`"

    ```text
    ===========================
    Total time (ms) : 443
    Nodes searched  : 1891018
    Nodes/second    : 4268663
    ```

=== "`koi-engine eval`"

    ```text
    Evaluator      : NNUE (v9 halfka-hm-v2, 7680->256x2->8)
    Side to move   : +0.20 (+20 cp)
    White relative : +0.20 (+20 cp)
    WDL (stm)      : 35.9% W / 29.5% D / 34.6% L
    Game phase     : 24/24 (pieces: 32)
    ```

=== "`koi-engine compiler`"

    ```text
    Koi 1.2.0 (<commit>)
    Compiled by    : AppleClang 21.0.0
    C++ standard   : C++26
    Architecture   : arm64
    SIMD / ISA     : NEON
    ```

---

## Interactive Session Commands

Inside a live `koi-engine` stdin session, standard UCI commands (`uci`, `isready`, `ucinewgame`, `position`, `setoption`, `go`, `stop`, `ponderhit`, `debug`, `quit`) are paired with interactive inspection commands:

| Command | Arguments | Purpose |
| :--- | :--- | :--- |
| `d` | — | Display ASCII chessboard, FEN, 64-bit Zobrist key, side, check state, and legal move count |
| `fen` | — | Print the six-field FEN of the current board |
| `key` | — | Print hexadecimal 64-bit Zobrist position, pawn hash, and Polyglot book keys |
| `moves` | — | List all legal moves sorted lexicographically in UCI coordinate notation |
| `book` | — | Probe the active Polyglot book for all legal weighted entries and selection percentages |
| `tb` | — | Probe loaded Syzygy tablebases for root WDL classification and optimal moves |
| `eval` | — | Print active evaluator architecture (`NNUE` or `HCE`), score (stm & White), WDL, and phase |
| `flip` | — | Toggle side to move when legal (clears en passant; rejects positions in check) |
| `see` | `<uci-move>` | Compute bitboard Static Exchange Evaluation (SEE) gain for a legal move in centipawns |
| `perft` | `<depth>` | Run an interruptible perft divide (`1..10`) from the current position |
| `bench` | `[depth [hash [threads]]]` | Run the 16-position deterministic benchmark inside the active session |
| `wait` | — | Block until any finite active search or `perft` finishes (useful in piped transcripts) |
| `compiler` | — | Print engine version, git commit, compiler, C++26 standard, architecture, and ISA |
| `help` | — | Print the interactive command summary |

### Example Interactive Session

```text
position fen 8/8/8/8/8/4k3/4P3/4K3 w - - 0 1
key
Position key : <64-bit hex>
Pawn key     : <64-bit hex>
Polyglot key : <64-bit hex>
tb
Syzygy : draw (+0 cp) | moves: e1d1 e1f1
see e1d1
SEE e1d1 : +0 cp
```

See the [User Guide](USER_GUIDE.md#uci-options-quick-reference) for the complete table of 27 advertised UCI options and the [Engine Specification](ENGINE_SPEC.md#7-uci-surface) for `CompletionGate` and bounded line guarantees.

---

## Multi-Protocol Modes

Koi targets standard 8×8 chess across five wire dialects, selectable via `--protocol <name>`, `KOI_PROTOCOL=<name>`, or `setoption name Protocol value <name>`:

- `uci` (default) — Universal Chess Interface (`uciok`, `bestmove <uci>`).
- `xboard` — XBoard / CECP v2 (`feature ... done=1`, `setboard`, `usermove`, `ping`/`pong`, `level`/`st`/`sd`, `analyze`/`exit`, `undo`/`remove`).
- `usi` — USI compatibility dialect on 8×8 boards (`usiok`, `usinewgame`, `byoyomi`, `bestmove resign`).
- `ucci` / `ucicyclone` — UCCI compatibility dialects on 8×8 boards (`ucciok`, `uccinewgame`, `time`/`opptime`/`increment`, `nobestmove`).

---

## Companion Developer Tools

Built from source via CMake presets (not shipped in user-facing release archives; every binary supports `-h` / `--help`):

| Binary / Script | Role | Usage |
| :--- | :--- | :--- |
| `koi-bench` | 64-position tactical & 128-position positional strength suite | `koi-bench --nnue NET [--optional] [--timed] [--depth-sweep a..b] [--report PATH]` |
| `koi-perft` | Fast legal move-generator perft & `--divide` counter | `koi-perft [--divide] <depth> [fen]` |
| `koi-replay` | Coordinate move replayer (`legal`, `result`, `termination`, `fen`) | `koi-replay <startpos\|fen 6-field-fen> [moves <uci>...]` |
| `koi-eval-features` | Bulk CSV evaluator dumper for FEN corpora | `koi-eval-features (--nnue NET \| --hce) [--input FILE] [--output FILE]` |
| `koi-nnue-fixture` | Synthetic v4/v5 `.nnue` generator for C++ & Python boundary tests | `koi-nnue-fixture --arch v4\|v5 --output <path>` |
| `tools/measurement/nnue_info.py` | Offline NNUE container inspector & SHA-256 payload verifier | `python tools/measurement/nnue_info.py <network.nnue>` |

For full measurement, SPRT matching, and packaging documentation, see [Developer Tools](../tools/README.md).

---

## Environment Variables

| Variable | Effect |
| :--- | :--- |
| `KOI_NNUE_PATH` | Explicit `.nnue` path loaded at boot (overrides auto-discovery) |
| `KOI_NNUE_AUTODETECT` | Set to `0` to disable automatic `koi.nnue` discovery beside the executable |
| `KOI_EVALUATOR` | Set to `hce` (or `classical`) to boot directly with the classical tapered evaluator |
| `KOI_CPU_VARIANT` | Override x86-64 ISA dispatch selector (`generic`, `avx2`, `avx512`) |
| `KOI_PROTOCOL` | Initial wire protocol (`uci`, `usi`, `ucci`, `ucicyclone`, `xboard`) |
