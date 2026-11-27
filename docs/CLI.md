# CLI & Interactive Reference

In addition to headless GUI communication, `koi-engine` and its companion developer binaries provide terminal commands for inspecting boards, hash keys, opening books, Syzygy tablebases, NNUE evaluations, perft counts, and deterministic benchmarks.

---

## `koi-engine` One-Shot CLI Commands

Run one-shot commands directly from a shell (`koi-engine.exe` on Windows, `koi-engine` on Linux and macOS):

```bash
# OpenBench-compatible deterministic benchmark (defaults: depth 10, 16 MB hash, 1 thread)
koi-engine bench [depth [hash [threads]]]
koi-engine --bench [depth [hash [threads]]]

# Timed multi-position NPS throughput benchmark (defaults: 1 thread, 128 MB hash, 3 seconds)
koi-engine speedtest [threads [hash [seconds]]]
koi-engine --speedtest [threads [hash [seconds]]]

# Build metadata, C++26 standard, and active SIMD ISA (no NNUE network required)
koi-engine compiler
koi-engine --compiler

# Native NNUE container metadata & SHA-256 payload checksum verifier
koi-engine nnue [path]

# Timed perft divide from startpos or custom FEN (depth 1..10; no NNUE network required)
koi-engine perft <depth> [fen]

# Zero-network board diagram, six-field FEN, Zobrist/Polyglot hash keys, legal moves, flip, book/TB probe, and SEE
koi-engine d [fen]
koi-engine --display [fen]
koi-engine fen [fen]
koi-engine key [fen]
koi-engine moves [fen]
koi-engine flip [fen]
koi-engine book [fen]                  # or --book <path> book [fen]
koi-engine tb [fen]                    # or --syzygy <dir> tb [fen]
koi-engine see <move> [fen]

# Static NNUE/HCE evaluation readout for startpos or custom FEN
koi-engine eval [fen]
koi-engine --eval [fen]

# Force built-in classical evaluation or select a specific NNUE network file
koi-engine --hce eval [fen]
koi-engine --nnue <path> eval [fen]

# Identity, GUI JSON profile, protocol dialect, batch execution, and help
koi-engine --version      # or -v
koi-engine --engine-json
koi-engine --protocol <uci|usi|ucci|ucicyclone|xboard>
koi-engine -c "position startpos moves e2e4" -c "go depth 6"   # repeatable --cmd
koi-engine --help         # or -h
```

!!! note "Zero-network subcommands, unquoted FENs & batch mode"
    `compiler`, `nnue`, `perft`, `d`, `fen`, `key`, `moves`, `flip`, `book`, `tb`, `see`, `--version`, `--help`, and `--engine-json` execute before normal engine boot (`nnue` inspects and verifies either a supplied `.nnue` file or the auto-discovered network). Both quoted (`koi-engine perft 2 "..."`), unquoted 6-field FEN arguments, and `[startpos|fen] [moves <uci>...]` sequences (for example `koi-engine fen moves e2e4 e7e5 g1f3` or `koi-engine eval startpos moves e2e4 e7e5`) are supported. Repeatable `-c <cmd>` / `--cmd <cmd>` runs protocol commands in batch mode (auto-appending `wait` + `quit` unless the last command is already `quit`/`bye`) and cannot be combined with one-shot subcommands. Invalid perft depths, `speedtest`/`bench` bounds, corrupt `.nnue` files, illegal `see`, `flip`, or `moves` inputs, missing `--book`/`--syzygy`/`--nnue`/`--cmd` values, malformed FENs, or unknown flags exit cleanly with status `2` and a diagnostic on `stderr`. On Linux and macOS, the root `Makefile` wraps these directly as `make profile-build` (PGO), `make smoke`, `make speedtest`, `make bench`, `make eval`, `make compiler`, and `make perft PERFT_DEPTH=5`.

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
    Network SHA256 : 6290b13822c3
    Output bucket  : 0 (of 8)
    Quantization   : int16/int8 (QA=255, QB=64, scale=400)
    ```

=== "`KOI_EVALUATOR=hce koi-engine eval` (Kiwipete)"

    ```text
    Evaluator      : HCE (classical tapered)
    Side to move   : +1.49 (+149 cp)
    White relative : +1.49 (+149 cp)
    WDL (stm)      : 42.3% W / 26.3% D / 31.4% L
    Game phase     : 24/24 (pieces: 32)

    Term           |      MG |      EG |   Total
    ---------------+---------+---------+--------
    Material       |   +0.00 |   +0.00 |  +0.00
    PST            |   +1.15 |   +0.30 |  +1.15
    Mobility       |   +0.10 |   +0.14 |  +0.10
    Pawns          |   +0.18 |   +0.24 |  +0.18
    King safety    |   -0.11 |   -0.06 |  -0.11
    Threats        |   +0.32 |   +0.29 |  +0.32
    Pieces         |   -0.25 |   -0.15 |  -0.25
    Space & dev    |   +0.00 |   +0.00 |  +0.00
    Tempo          |   +0.10 |   +0.10 |  +0.10
    ---------------+---------+---------+--------
    Total (White)  |   +1.49 |   +0.86 |  +1.49
    ```

=== "`koi-engine compiler`"

    ```text
    Koi 1.2.0 (<commit>)
    Compiled by    : AppleClang 21.0.0
    C++ standard   : C++26
    Architecture   : arm64
    SIMD / ISA     : NEON
    ```

=== "`koi-engine nnue`"

    ```text
    NNUE file      : koi-v9-hm10-h256-official.nnue
    Format version : v9
    Feature set    : halfka-hm-v2
    Architecture   : 7680 -> 256 x 2 -> 8
    Quantization   : int16/int8 (QA=255, QB=64, scale=400)
    Payload bytes  : 3937338
    Payload SHA256 : 6290b13822c3...
    Status         : valid (checksum verified)
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
| `nnue` | — | Print active loaded NNUE container metadata and payload SHA-256 checksum (or inactive HCE status) |
| `flip` | — | Toggle side to move when legal (clears en passant; rejects positions in check) |
| `see` | `<uci-move>` | Compute bitboard Static Exchange Evaluation (SEE) gain for a legal move in centipawns |
| `perft` | `<depth>` | Run an interruptible perft divide (`1..10`) with elapsed time and NPS from the current position |
| `bench` | `[depth [hash [threads]]]` | Run the 16-position deterministic benchmark inside the active session |
| `speedtest` | `[threads [hash [seconds]]]` | Run a timed multi-position NPS throughput benchmark (`seconds` in `1..60`) |
| `options` | — | Print live active protocol, evaluator, search, book, and Syzygy option values |
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
| `tools/measurement/nnue_info.py` | Offline NNUE container inspector & SHA-256 payload verifier | `python tools/measurement/nnue_info.py <network.nnue> [--json] [--verify]` |

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
