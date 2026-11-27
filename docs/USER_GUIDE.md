# Koi User Guide

**Koi** is a Windows x64, Linux x86-64, and macOS arm64 (Apple Silicon) UCI
chess engine for standard chess. Written in C++26 and process-tested against
**En Croissant** as the primary GUI workflow, it combines iterative-deepening
alpha-beta search and a persistent transposition table with a gated Koi-native
NNUE as the primary evaluator (and a clean hand-crafted classical evaluator
selectable via `--hce`, `EvalMode=hce`, or `KOI_EVALUATOR=hce`, plus `--nnue <path>` to load an explicit network). Search runs on a
cancellable outer worker: `Threads = 1` runs the deterministic serial search,
while `Threads > 1` starts Lazy SMP helper threads that search the same root
against the shared transposition table while the main thread publishes the
result. Lazy SMP is intentionally nondeterministic: the deterministic guarantee
is scoped to `Threads = 1`, and threaded runs are validated for legality and
coverage instead of byte equality. The UCI command loop remains responsive in
both configurations.

## Name

The engine is called **Koi**. Historical releases through v1.1.5 used the name
"Koi Engine" in package text and documentation; the v1.1.5+ handshake already
identifies as `Koi <version>` (e.g., `Koi 1.2.0`). Those historical releases are
not renamed. New builds identify themselves as **Koi** in the UCI handshake, and
this guide uses **Koi** throughout. Binary and archive file names keep the
`koi-engine` spelling so existing scripts keep working.

## Architecture

Koi keeps chess rules, search, evaluation, transposition tables, and UCI protocol formatting behind strict internal module boundaries, with `UciController` as the sole protocol writer and `Position` as the native rules authority. For the full module diagram, threading ownership, and the 1.0.0 architecture contract, see [System Architecture](ARCHITECTURE.md).

## Build prerequisites

### Quick start with CMake Presets

Standardized Release builds across Windows, Linux, and macOS use [CMakePresets.json](../CMakePresets.json):

```bash
# Windows x64 (from an x64 Visual Studio Developer PowerShell)
cmake --preset windows-release && cmake --build --preset windows-release && ctest --preset windows-release

# Linux x86-64 (GCC 14; or use linux-clang-release)
cmake --preset linux-gcc-release && cmake --build --preset linux-gcc-release && ctest --preset linux-gcc-release

# macOS arm64 (Apple Silicon)
cmake --preset macos-arm64-release && cmake --build --preset macos-arm64-release && ctest --preset macos-arm64-release
```

Available presets in [CMakePresets.json](../CMakePresets.json) include `windows-release`, `windows-debug`, `windows-asan`, `linux-gcc-release`, `linux-clang-release`, `linux-gcc-tsan`, `macos-arm64-release`, and `macos-arm64-debug`. On Linux and macOS you can also use the OpenBench-compatible [Makefile](../Makefile) (`make`, `make tools`, `make profile-build` for PGO, `make EXE=koi-engine`, `make smoke`, `make speedtest`, `make bench`, `make nnue`, `make options`, `make eval`, `make d`, `make version`, `make compiler`, `make perft`, `make test`, `make docs`). Explicit toolchain requirements and manual CMake invocations follow below:

=== "Windows x64"

    - A C++26-capable x64 MSVC toolchain (the CMake configuration selects Visual Studio's `/std:c++latest` compiler mode). Run CMake from an x64 Native Tools Command Prompt or x64 Developer PowerShell for Visual Studio, so `cl.exe` is selected; MinGW/GCC/Clang are not supported on Windows.
    - CMake 3.31 or newer.
    - Ninja.

    ```powershell
    cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
    cmake --build build\release --config Release
    ctest --test-dir build\release -C Release -j 8 --output-on-failure
    ```

=== "Linux x86-64"

    - GCC 14+ or Clang 18+ (the CMake configuration requires one of the two and selects `-std=c++26` / `-std=c++2c`).
    - CMake 3.31 or newer. Distributions such as Ubuntu 24.04 ship an older CMake, so install a newer one first, for example with `python3 -m pip install --upgrade cmake`, `pipx install cmake`, or the Kitware APT repository.
    - Ninja (required for the C++26 named-module build) and a `pthreads` libc.
    - Optional: `pwsh` for the PowerShell process tests and Python 3 for the Python tooling tests.

    ```bash
    cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14
    cmake --build build/release --config Release
    ctest --test-dir build/release -C Release -j 8 --output-on-failure
    ```

=== "macOS arm64 (Apple Silicon)"

    - Homebrew LLVM (Clang 19+) is the recommended C++26 toolchain; AppleClang 18+ also works. Install the compiler and Ninja with `brew install llvm ninja`.
    - CMake 3.31 or newer, for example `python3 -m pip install --upgrade cmake`.
    - C++26 named modules are disabled on macOS (`-DKOI_BUILD_MODULES=OFF`), and the Linux static-runtime policy does not apply (`-DKOI_STATIC_RUNTIME=OFF`). The engine is CPU-only and uses the CPU NNUE evaluator by default (unless `--hce`, `KOI_EVALUATOR=hce`, or `EvalMode=hce` selects the clean classical evaluator).
    - Optional: `pwsh` for the PowerShell process tests and Python 3 for the Python tooling tests.

    ```bash
    brew install llvm ninja
    cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang" \
      -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++" \
      -DKOI_BUILD_MODULES=OFF -DKOI_STATIC_RUNTIME=OFF
    cmake --build build/release --config Release
    ctest --test-dir build/release -C Release -j 8 --output-on-failure
    ```

C++26 named modules are auto-detected: they are built with MSVC, Clang 18+,
and GCC 14+, and the CMake option `KOI_BUILD_MODULES=OFF` skips them (and the
module test) on compilers where they are not usable; the documented macOS
arm64 build passes that fallback explicitly. The Linux build links
libstdc++/libgcc statically by default for a portable binary; pass
`-DKOI_STATIC_RUNTIME=OFF` to link the system runtime instead (the macOS build
has no static-runtime policy and always passes it).

For a Debug build, substitute `build\debug` and `Debug` in those commands. The
Release build produces three engine executables in `build\release`
(`build/release` on Linux and macOS, where they carry no `.exe` suffix; macOS
arm64 builds only the baseline `koi-engine`):

- `koi-engine.exe` — the baseline build and the single entry point for GUIs and
  scripts. It starts the fastest sibling this CPU supports
  (`koi-engine-avx512.exe`, then `koi-engine-avx2.exe`) when that file is
  present, and otherwise runs its own baseline code in place. The selector
  writes nothing to stdout or stderr.
- `koi-engine-avx2.exe` — Release-only `/arch:AVX2` build.
- `koi-engine-avx512.exe` — Release-only `/arch:AVX512` build.

Each optimized sibling checks the CPU at startup and exits with code 3 and a
clear message instead of executing an unsupported instruction. Set
`KOI_CPU_VARIANT=generic`, `avx2`, or `avx512` to force one build (`auto` and
unknown values keep the automatic behavior). On Windows the selector launches
the sibling as a child process, forwards its exit code, and a job object kills
the child if the parent disappears; on Linux it `execv`s the sibling so the
process image is replaced. On macOS arm64 there are no AVX siblings, so the
baseline `koi-engine` runs directly. All other developer tools (`koi-bench`,
`koi-replay`, `koi-perft`, `koi-eval-features`) are built with AVX2 on Windows
and Linux x86-64 and as baseline binaries on macOS arm64.

For an independently reproducible release gate, run the checked-in harness from
an x64 Visual Studio developer shell. It configures and builds fresh canonical
Debug and Release trees, runs all CTest/process tests, checks tactical
Threads 1/2/4 when the host supports them (with an explicit maximum-thread
fallback), and writes benchmark, UCI, replay, and En Croissant-style artifacts
under `artifacts/verification`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build\release_verify.ps1 `
  -NnuePath .\artifacts\networks\koi-v9-hm10-h256-official.nnue `
  -CMakePath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" `
  -OutputDirectory .\artifacts\verification\release-verify
```

The harness records the exact Debug/Release configure, build, CTest, benchmark,
transcript, replay, and match command outputs under the supplied artifact
directory. It makes no
Stockfish/CPL or En Croissant GUI availability assumption.

## Developer tools

From the configured build directory:

=== "Windows (PowerShell)"

    ```powershell
    .\build\release\koi-perft.exe 4
    .\build\release\koi-perft.exe --divide 3 "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"
    .\build\release\koi-bench.exe --nnue .\artifacts\networks\koi-v9-hm10-h256-official.nnue --threads 4 --speed 100 --timed
    .\build\release\koi-bench.exe --nnue .\artifacts\networks\koi-v9-hm10-h256-official.nnue --optional --profile-json .\artifacts\verification\optional-strength.json
    .\build\release\koi-bench.exe --nnue .\artifacts\networks\koi-v9-hm10-h256-official.nnue --nodes 20000 --timed --warm-hash --warmup 1 --repeat 5 --profile-json .\artifacts\verification\steady-state.json
    .\build\release\koi-replay.exe startpos moves e2e4 e7e5 g1f3
    ```

=== "Linux / macOS (Bash)"

    ```bash
    ./build/release/koi-perft 4
    ./build/release/koi-perft --divide 3 "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"
    ./build/release/koi-bench --nnue ./artifacts/networks/koi-v9-hm10-h256-official.nnue --threads 4 --speed 100 --timed
    ./build/release/koi-bench --nnue ./artifacts/networks/koi-v9-hm10-h256-official.nnue --optional --profile-json ./artifacts/verification/optional-strength.json
    ./build/release/koi-bench --nnue ./artifacts/networks/koi-v9-hm10-h256-official.nnue --nodes 20000 --timed --warm-hash --warmup 1 --repeat 5 --profile-json ./artifacts/verification/steady-state.json
    ./build/release/koi-replay startpos moves e2e4 e7e5 g1f3
    ```

`koi-perft` counts legal nodes from `startpos` (or `[startpos|fen [moves <uci>...]]`) at
the given non-negative depth using bulk counting and mirror-detached root traversal (`--divide` prints per-move counts, elapsed time,
and NPS; pass `--help` on `koi-perft`, `koi-replay`, or `koi-bench` for full
usage). `koi-bench` runs the 64-position fixed-depth tactical hard gate and
writes only its deterministic benchmark report to stdout by default (`--nnue <path>`
selects the NNUE weights or `--hce` selects the built-in classical evaluator).
`--optional` instead selects the 128-position optional strength corpus and labels
that suite in both its text and JSON-profile output. `--threads N` and `--speed
1..100` are recorded in every report and use the same deterministic search
configuration as the corresponding engine controls. Every text report includes
`hash cold` or `hash warm`; `--warm-hash` reuses one search service across rows
and is useful for comparing warmed-table behavior. The default is cold.
`--nodes N` replaces the per-position fixed depth with a node limit, matching the
node-limited match regime; `--warmup K` runs K unmeasured suite passes first and
`--repeat K` keeps K measured samples per position, reporting the median-time
sample as the primary result and retaining every sample in the profile's `runs`
array. Combine all three for steady-state throughput comparisons. `--timed` is
opt-in and adds wall-clock `elapsed_ms` and measured NPS to text and JSON; it is
intentionally absent from the default CI-shaped output. Untimed JSON profiles use
the stable `Koi 1.2.0` build identity, set `timed` to `false`, and record
`nps` as unmeasured (`0`). Each profile carries `hash_state` (`cold` or `warm`) at
the top level and on every position. It is a separate process and never writes to
the UCI engine's stdout.

`koi-replay` is a separate rules-boundary tool for replaying coordinate moves without
exposing the vendored chess library. Give it `startpos` or `fen <six-field FEN>`, then
an optional `moves` list. Its stable stdout reports `legal`, `result`, `termination`,
and the final six-field `fen`; an illegal move leaves the reported position at the
last legal state. It is useful for reproducing a match-ply or validating a UCI log.

The Stockfish position oracle is a measurement-only Python tool and does not add a
dependency to the C++ engine. Install the measurement dependencies and run its test
directly from the repository root:

=== "Windows (PowerShell)"

    ```powershell
    python -m pip install -r .\tools\measurement\requirements.txt
    python -m unittest .\tests\python\measurement\elo_oracle_test.py -v
    ```

=== "Linux & macOS"

    ```bash
    python3 -m pip install -r ./tools/measurement/requirements.txt
    PYTHONPATH=. python3 ./tests/python/measurement/elo_oracle_test.py -v
    ```

See [`tools/README.md`](../tools/README.md) for the extract-only schema, the named
book-audit and match entrypoints, and the external-results workflow.

When Python 3 is available at CMake configure time, the same test is registered as
`elo_oracle_python` in CTest. The in-tree `koi_chess` package supplies the rules,
PGN, and UCI tooling, so no third-party chess package is required.

To analyze a supplied standard-SAN PGN with Stockfish as the position oracle, keep the
JSON output under this checkout's artifacts directory and provide the exact
executable/version and PGN provenance alongside the report (use `--koi-eval-file <path.nnue>`
or `--hce` to select Koi's evaluator):

=== "Windows (PowerShell)"

    ```powershell
    python .\tools\measurement\elo_oracle.py `
      --pgn .\tests\data\games\2026-09-08-koi-vs-stockfish-19.pgn `
      --koi .\build\release\koi-engine.exe `
      --koi-eval-file .\artifacts\networks\koi-v9-hm10-h256-official.nnue `
      --stockfish .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
      --output .\artifacts\verification\elo-oracle.json `
      --movetime-ms 250 --threads 4
    ```

=== "Linux & macOS"

    ```bash
    python3 ./tools/measurement/elo_oracle.py \
      --pgn ./tests/data/games/2026-09-08-koi-vs-stockfish-19.pgn \
      --koi ./build/release/koi-engine \
      --koi-eval-file ./artifacts/networks/koi-v9-hm10-h256-official.nnue \
      --stockfish /path/to/stockfish \
      --output ./artifacts/verification/elo-oracle.json \
      --movetime-ms 250 --threads 4
    ```

Use `--extract-only` to test PGN/FEN extraction without either engine. Run the licensed
opening book as a separate audit so its `book_used`, `book_move`, and Stockfish CPL are
kept outside the normal search metrics:

=== "Windows (PowerShell)"

    ```powershell
    python .\tools\measurement\elo_oracle.py `
      --pgn .\tests\data\games\2026-09-08-koi-vs-stockfish-19.pgn `
      --koi .\build\release\koi-engine.exe `
      --koi-eval-file .\artifacts\networks\koi-v9-hm10-h256-official.nnue `
      --stockfish .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
      --book C:\LicensedBooks\book.bin --book-audit `
      --output .\artifacts\verification\book-audit.json `
      --movetime-ms 250 --threads 4
    ```

=== "Linux & macOS"

    ```bash
    python3 ./tools/measurement/elo_oracle.py \
      --pgn ./tests/data/games/2026-09-08-koi-vs-stockfish-19.pgn \
      --koi ./build/release/koi-engine \
      --koi-eval-file ./artifacts/networks/koi-v9-hm10-h256-official.nnue \
      --stockfish /path/to/stockfish \
      --book /path/to/licensed/book.bin --book-audit \
      --output ./artifacts/verification/book-audit.json \
      --movetime-ms 250 --threads 4
    ```

The PGN, Stockfish executable/version, and licensed `book.bin` are external inputs;
none are assumed to exist in this repository. Do not report an Elo or CPL improvement
until both a comparable baseline and an after-change report have been generated.

The offline Koi-native boundary fixtures (`train_nnue.py` and `export_nnue.py`, supporting deterministic `--backend synthetic`) emit `piece-square-king-pawn-v2` containers for unit/contract tests while production networks use NNUE v9 (`halfka-hm-v2`); neither adds a Python runtime dependency to the engine:

=== "Windows (PowerShell)"

    ```powershell
    python .\tools\measurement\train_nnue.py --help
    python .\tools\measurement\export_nnue.py --help
    ```

=== "Linux & macOS"

    ```bash
    python3 ./tools/measurement/train_nnue.py --help
    python3 ./tools/measurement/export_nnue.py --help
    ```

`--backend synthetic` is deterministic and suitable for boundary tests. The optional
PyTorch backend is offline-only, records corpus and network provenance, and must pass
the Koi loader and strength gates before any network is considered for runtime use.

For GPU-accelerated **NNUE v9** (`halfka-hm-v2`, 10 mirrored king buckets, 256×2 SCReLU) and CPU PyTorch training pipelines—corpus generation, encoding, quantization, SPRT gates, and promotion rules—see [NNUE Pipeline](NNUE_PIPELINE.md):

=== "Windows (PowerShell)"

    ```powershell
    # Self-play data generation and Bullet / PyTorch training workflow
    python .\tools\measurement\gen_training_data.py all --games 30000 --workers 3 --label-depth 10
    python .\tools\nnue\to_bullet.py --input .\artifacts\training\labels.txt --output .\artifacts\training\koi.data
    .\build\release\koi-bench.exe --nnue .\artifacts\training\koi-candidate.nnue
    ```

=== "Linux & macOS"

    ```bash
    # Self-play data generation and Bullet / PyTorch training workflow
    python3 ./tools/measurement/gen_training_data.py all --games 30000 --workers 3 --label-depth 10
    python3 ./tools/nnue/to_bullet.py --input ./artifacts/training/labels.txt --output ./artifacts/training/koi.data
    ./build/release/koi-bench --nnue ./artifacts/training/koi-candidate.nnue
    ```

The engine discovers a network automatically at startup: it looks for `koi.nnue` beside the executable, then `nnue/koi.nnue`, then a single `*.nnue` file in either directory (and in the working directory for development runs). Release archives bundle the promoted gated network flat as `koi.nnue` beside the executables; when discovered at boot, the UCI handshake advertises `EvalFile` default `<autodiscovery>`. Point `KOI_NNUE_PATH` at a specific file to override the scan, or set `KOI_NNUE_AUTODETECT=0` to disable discovery. A missing or rejected network at boot exits 4 with one diagnostic on stderr (unless `KOI_EVALUATOR=hce` is set), while runtime `EvalFile` swaps fail closed without dropping the previous evaluator. Standalone official networks are hosted on the [Koi NNUEs](https://github.com/nhatquang1701/Koi/releases/tag/koi-nnues) pre-release.

### Training a network

Networks are trained with the Bullet GPU trainer (`tools/nnue/run_bullet.py`) or
the PyTorch CPU trainer (`tools/measurement/train_nnue_koi.py`):

=== "Windows (PowerShell)"

    ```powershell
    python .\tools\nnue\run_bullet.py --help
    pwsh -NoProfile -File .\tools\nnue\net_match.ps1 -NnueNet .\artifacts\training\candidate.nnue `
      -OpponentNet .\artifacts\networks\koi-v9-hm10-h256-official.nnue -Games 160
    ```

=== "Linux & macOS"

    ```bash
    python3 ./tools/nnue/run_bullet.py --help
    pwsh -NoProfile -File ./tools/nnue/net_match.ps1 -NnueNet ./artifacts/training/candidate.nnue \
      -OpponentNet ./artifacts/networks/koi-v9-hm10-h256-official.nnue -Games 160
    ```

Validation runs the 64-position `koi-bench --nnue` gate and a colour-split
network-vs-network match (`tools/nnue/net_match.ps1`, schema
`koi-nnue-net-match-v1`), or `tools/stability/uci_match.ps1` with
`-OpponentOptions @{ EvalMode = 'hce' }` against the clean classical evaluator.
Validation output is a local report, not an Elo
claim or a CI threshold. Use `Threads` to configure multi-threaded CPU search.

For a reproducible local match against Stockfish or another UCI engine, use the
optional PowerShell harness (PowerShell 7 `pwsh` works on Windows, Linux, and macOS):

=== "Windows (PowerShell)"

    ```powershell
    .\tools\stability\uci_match.ps1 `
      -KoiPath .\build\release\koi-engine.exe `
      -OpponentPath .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
      -Depth 6 -Threads 4 -Hash 512 `
      -OutputDirectory .\artifacts\matches\match-results
    ```

=== "Linux & macOS"

    ```bash
    pwsh -NoProfile -File ./tools/stability/uci_match.ps1 \
      -KoiPath ./build/release/koi-engine \
      -OpponentPath /path/to/stockfish \
      -Depth 6 -Threads 4 -Hash 512 \
      -OutputDirectory ./artifacts/matches/match-results
    ```

The harness writes a `koi-uci-match-v2` JSON report plus matching `.pgn`. It records
both engines' UCI handshakes and options; every ply's actual root FEN, exact
`position` and `go` commands, engine label, returned move, replay legality, timing,
parsed final info/PV, all info lines, and raw `bestmove`; and each game's adjudicated
result, winner, termination, and process status. Moves are replay-validated before
they are appended, so illegal moves and non-terminal `0000` replies end only that game
without contaminating later positions. PGN Result headers match the adjudicated result
(`*` for max-ply games) and retain the `MoveFormat` header. Use `-MovetimeMs` or
`-Nodes` instead of `-Depth` for those limits. To run a named FEN suite, pass
`-FenFile` containing one entry per line in the form `name | six-field FEN` (blank
lines and `#` comments are ignored). The replay executable is expected beside
`koi-engine.exe` (or can be supplied as `-ReplayPath`).

For color-balanced paired Elo measurements, supply the checked-in eight-opening
suite (or another file in the same `name | uci move uci move` format) and a chess
clock. `-Games` is the number of games run for each selected opening. Koi's
`RandomSeed` only seeds opening-book choice, so deterministic repeated games on
one opening replay identical moves: the JSON configuration counts distinct move
sequences as `effective_games` and sets `duplicate_games_detected` with
`report_valid: false` when a match repeats a game. Prefer one game per opening
per color (separate Koi White and Koi Black runs) or add `-PairedColors` to
alternate Koi's color between paired games in one run (the second game of each
pair plays the opposite color). Run both
book-disabled and licensed-book conditions at both clocks:

=== "Windows (PowerShell)"

    ```powershell
    # No-book 1+0 color-paired match
    .\tools\stability\uci_match.ps1 `
      -KoiPath .\build\release\koi-engine.exe `
      -OpponentPath .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
      -OpeningFile .\tests\data\openings\openings-basic.txt `
      -TimeControl 1+0 -Games 20 -PairedColors `
      -KoiRandomSeed 1 -KoiOwnBook false `
      -OutputDirectory .\artifacts\matches\no-book-1p0-paired

    # With a licensed book at 5+3
    .\tools\stability\uci_match.ps1 `
      -KoiPath .\build\release\koi-engine.exe `
      -OpponentPath .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
      -OpeningFile .\tests\data\openings\openings-basic.txt `
      -TimeControl 5+3 -Games 20 -PairedColors `
      -KoiRandomSeed 1 -KoiOwnBook true -KoiBookFile C:\LicensedBooks\book.bin -KoiBookDepth 16 `
      -OutputDirectory .\artifacts\matches\book-5p3-paired
    ```

=== "Linux & macOS"

    ```bash
    # No-book 1+0 color-paired match
    pwsh -NoProfile -File ./tools/stability/uci_match.ps1 \
      -KoiPath ./build/release/koi-engine \
      -OpponentPath /path/to/stockfish \
      -OpeningFile ./tests/data/openings/openings-basic.txt \
      -TimeControl 1+0 -Games 20 -PairedColors \
      -KoiRandomSeed 1 -KoiOwnBook false \
      -OutputDirectory ./artifacts/matches/no-book-1p0-paired

    # With a licensed book at 5+3
    pwsh -NoProfile -File ./tools/stability/uci_match.ps1 \
      -KoiPath ./build/release/koi-engine \
      -OpponentPath /path/to/stockfish \
      -OpeningFile ./tests/data/openings/openings-basic.txt \
      -TimeControl 5+3 -Games 20 -PairedColors \
      -KoiRandomSeed 1 -KoiOwnBook true -KoiBookFile /path/to/book.bin -KoiBookDepth 16 \
      -OutputDirectory ./artifacts/matches/book-5p3-paired
    ```

`-TimeControl` accepts only `<minutes>+<increment>` (for example `1+0` or `5+3`)
and sends `wtime`, `btime`, `winc`, and `binc` on every `go`; elapsed time is deducted
from the moving side before its increment is added. Opening sequences are replayed and
rejected before either engine starts a game. The JSON configuration records the clock
and Koi `RandomSeed`, `OwnBook`, `BookFile`, and `BookDepth` values. Per-ply
`book_used` and `book_move` record an `info string book move <uci> depth <ply>` marker
separately from normal search PV data. The harness sends Koi's book options without
requiring a book reader or any Jack dependency; Koi versions predating book support
ignore those UCI options safely.

`tools/stability/sprt_compare.ps1` wraps `uci_match.ps1` for candidate-versus-baseline
acceptance testing: it launches the white and black SPRT runs concurrently, reads both
reports, and prints per-color and combined results. The combined decision accepts when
the sum of the two color LLRs crosses the alpha = beta = 0.05 acceptance bound,
and rejects when it crosses the rejection bound. Both Koi binaries start from seed 0
with their opening books disabled, and every repeated game derives its own seed.
The tool defaults to one game per opening and color; repeated move sequences are
counted once as `effective_games` and raise a loud duplicate-game warning. The script exits 0 on
accept, 1 on reject, and 2 when more games are needed.
The v1.2.0 release comparison uses the fixed, color-reversed opening sample and
exact bound described in the release evidence report; this SPRT utility is exploratory.

=== "Windows (PowerShell)"

    ```powershell
    .\tools\stability\sprt_compare.ps1 `
      -CandidatePath .\build\release\koi-engine.exe `
      -BaselinePath .\artifacts\verification\baseline\koi-engine-baseline.exe `
      -Nodes 20000 -Games 2 -MinGames 40 -MaxGames 64 -Elo1 5 `
      -OutputDirectory .\artifacts\matches\sprt-candidate
    ```

=== "Linux & macOS"

    ```bash
    pwsh -NoProfile -File ./tools/stability/sprt_compare.ps1 \
      -CandidatePath ./build/release/koi-engine \
      -BaselinePath ./artifacts/verification/baseline/koi-engine-baseline \
      -Nodes 20000 -Games 2 -MinGames 40 -MaxGames 64 -Elo1 5 \
      -OutputDirectory ./artifacts/matches/sprt-candidate
    ```

## Rough local Elo measurement

`tools/measurement/elo_estimate.py` is a standard-library-only, measurement-only harness. Its
primary configuration is no-book, 1+0, Koi `Hash=512` and `Threads=4`, with 32
named opening lines played once with Koi White and once with
Koi Black at each Stockfish anchor. The initial schedule is two 64-game anchor
batches (128 games); adaptive batches may extend the run to 192, 256, or 320 games.
The estimator uses 2,000 paired-opening bootstrap samples and rejects incomplete,
illegal, timed-out, or malformed match evidence.

The default schedule samples the two anchors around `--prior-elo`. Use
`--all-anchors` for a calibration sweep: it runs one initial 64-game paired
batch for every anchor in the manifest and writes a version-2 schedule record.
Set `--max-games` to at least 64 times the number of anchors; further batches
remain adaptive.

Create an external `koi-elo-anchor-manifest-v1` JSON manifest. Its `stockfish`
object must contain the existing executable `path`, at least two unique
`elos` in the Stockfish `UCI_Elo` range 1320..3190, and a `rating_source` such as
`Stockfish 19 UCI_LimitStrength`. Each optional `lower_anchors` entry must contain
an `id`, existing executable `path`, positive `rating`, and `rating_source`; a
lower anchor is required when `--prior-elo` is below the lowest Stockfish anchor.
The manifest must bracket `--prior-elo`, and `--stockfish` must match its
`stockfish.path`.

Run the primary no-book dry-run from the repository root with every executable,
manifest, corpus, and report path resolved explicitly:

=== "Windows (PowerShell)"

    ```powershell
    python .\tools\measurement\elo_estimate.py `
      --koi .\build\release\koi-engine.exe `
      --replay .\build\release\koi-replay.exe `
      --stockfish .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
      --anchors .\artifacts\manifests\elo-anchors.json `
      --openings .\tests\data\openings\openings-curated-32.txt `
      --time-control 1+0 `
      --threads 4 --hash 512 --speed 100 `
      --min-games 128 --max-games 320 `
      --prior-elo 1600 --mode no-book `
      --output .\artifacts\matches\rough-elo-no-book.json `
      --dry-run
    ```

=== "Linux & macOS"

    ```bash
    python3 ./tools/measurement/elo_estimate.py \
      --koi ./build/release/koi-engine \
      --replay ./build/release/koi-replay \
      --stockfish /path/to/stockfish \
      --anchors ./artifacts/manifests/elo-anchors.json \
      --openings ./tests/data/openings/openings-curated-32.txt \
      --time-control 1+0 \
      --threads 4 --hash 512 --speed 100 \
      --min-games 128 --max-games 320 \
      --prior-elo 1600 --mode no-book \
      --output ./artifacts/matches/rough-elo-no-book.json \
      --dry-run
    ```

`--dry-run` validates the executable paths, anchor manifest, 32-opening corpus,
required fixed options, and output location, writes the complete deterministic
schedule to the repository-local JSON report, and launches no engines. Remove only
`--dry-run` for a real run after supplying valid local Koi, replay, Stockfish,
anchor, corpus, and artifact paths. The real run writes raw `koi-uci-match-v2`
JSON and matching PGN artifacts below a sibling `<report-stem>-artifacts` directory
under `artifacts/matches`.

Book mode is a separate measurement and must not be combined with the primary
no-book result. Use a separate output path and licensed external book:

=== "Windows (PowerShell)"

    ```powershell
    python .\tools\measurement\elo_estimate.py `
      --koi .\build\release\koi-engine.exe `
      --replay .\build\release\koi-replay.exe `
      --stockfish .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
      --anchors .\artifacts\manifests\elo-anchors.json `
      --openings .\tests\data\openings\openings-curated-32.txt `
      --time-control 1+0 `
      --threads 4 --hash 512 --speed 100 `
      --min-games 128 --max-games 320 `
      --prior-elo 1600 --mode book --book C:\LicensedBooks\book.bin `
      --output .\artifacts\matches\rough-elo-book.json
    ```

=== "Linux & macOS"

    ```bash
    python3 ./tools/measurement/elo_estimate.py \
      --koi ./build/release/koi-engine \
      --replay ./build/release/koi-replay \
      --stockfish /path/to/stockfish \
      --anchors ./artifacts/manifests/elo-anchors.json \
      --openings ./tests/data/openings/openings-curated-32.txt \
      --time-control 1+0 \
      --threads 4 --hash 512 --speed 100 \
      --min-games 128 --max-games 320 \
      --prior-elo 1600 --mode book --book /path/to/book.bin \
      --output ./artifacts/matches/rough-elo-book.json
    ```

To measure a Koi NNUE network, pass `--koi-eval-file` with the network path. The
tool reads the container header, selects the matching `nnue-v4`/`nnue-v5`/`nnue-v6`/`nnue-v8`/`nnue-v9`
attestation, and requires the match harness to confirm that the engine enabled
that exact file; the report records the absolute path, SHA-256, byte size,
container version, and attestation under `configuration.koi_evaluator`. Missing, malformed, or rejected networks fail with a nonzero exit.
`--koi-options-json` forwards extra non-pinned Koi options to the harness and
records the file hash; it must not set `EvalFile`.

CTest registers the standard-library-only estimator, match-option, and opening
corpus tests when Python 3 is available; the corpus test receives the built
`koi-replay` path through `KOI_REPLAY_PATH`. These tests validate tooling and
corpus integration only. Real engine matches are intentionally unsuitable for a
fixed CI schedule because they depend on supplied external executables, hardware,
and time control; CTest is not an Elo threshold. Label any resulting figure as
“local Stockfish-equivalent Elo at recorded hardware/options/time control”; it is
not a universal Elo claim.

## Strength regression suite

The deterministic `Threads=1` reference path includes a fixed-depth
64-case tactical hard gate and a separate 128-case optional positional corpus. The
hard gate covers mates, checks, evasions, forks, pins, poisoned captures, promotions,
defensive choices, and pawn-race motifs, plus the retained legacy queen-capture case.
Each fixture records a stable ID, distinct legal FEN, depth,
category, and explicit accepted-move allowlist; multi-solution positions do not depend
on one arbitrary root tie-break.

Run the hard gate and the complete Release suite from your configured build directory:

=== "Windows (PowerShell)"

    ```powershell
    .\build\release\koi_strength_tests.exe
    ctest --test-dir build\release -C Release --output-on-failure -j
    ```

=== "Linux / macOS (Bash)"

    ```bash
    ./build/release/koi_strength_tests
    ctest --test-dir build/release --output-on-failure -j
    ```

The optional corpus is retained for local tuning and is deliberately not an Elo or NPS
CI threshold. Historical note: NNUE was once opt-in; since 2026-10-28 it is required;
Syzygy tablebases are optional, and chess variants remain out of scope. Opening-book
defaults, placement, fallback, and bypass behavior are documented below. This engine
continues to evaluate standard FIDE chess.

### Test inventory and known gaps

The reference Windows Release configuration with Python 3 and Cute Chess registers
87 CTest tests (`86` without `cutechess_stability_smoke`; `KOI_BUILD_SHADOW_DIFF=OFF`);
enabling the shadow-diff oracle adds `koi_shadow_diff_tests`, so the count
varies with those optional pieces. Run `ctest -N`
on the actual build instead of relying on a fixed number. Continuous
integration runs the suite on Windows (`.github/workflows/windows.yml`),
Linux with GCC 14 and Clang 18 (`.github/workflows/linux.yml`), and macOS
arm64 (`.github/workflows/macos.yml`). `koi_search_tests`, the heaviest suite, is
registered as four shards, every test carries labels (`unit`, `integration`,
`process`, `python`, `heavy`, and focused sub-labels), and the whole suite runs
in parallel (`ctest -j`; `tools/test/run_tests.ps1` builds, runs it with JUnit
and `LastTest.log` capture, and retries transient host crashes).
`tests/README.md` documents the
full inventory, how to run the whole suite or a single test (`KOI_TEST_FILTER`,
`--filter`, `--shard`), the environment variables (`KOI_TEST_RETRIES`,
`KOI_ALLOW_XPASS`, `KOI_TEST_TIMEOUT_SECONDS`, `KOI_UCI_TIMEOUT_MS`,
`KOI_REPLAY_PATH`, `PYTHONDONTWRITEBYTECODE`, `KOI_NNUE_BOUNDARY_EXE`),
optional-dependency skips, and per-test timeouts. The search suite's
`known_failures` and `intermittent` registries are empty; all search behavior
cases run as ordinary fatal tests.

## Interactive and CLI commands

In addition to standard UCI/XBoard sessions, `koi-engine` supports standard chess-engine CLI options and interactive terminal commands:

- **CLI subcommands & flags:** `koi-engine -c <cmd>` (`--cmd <cmd>`, repeatable batch execution with automatic `wait` + `quit`), `koi-engine bench [depth [hash [threads]]]` (or `--bench`, OpenBench-compatible deterministic benchmark), `koi-engine speedtest [threads [hash [seconds]]]` (or `--speedtest`, timed multi-position NPS throughput benchmark), `koi-engine compiler` (`--compiler`), `koi-engine nnue [path]` (zero-Python container metadata & payload SHA-256 verification), `koi-engine d`/`--display [pos]`, `koi-engine fen [pos]`, `koi-engine key [pos]`, `koi-engine moves [pos]`, `koi-engine flip [pos]`, `koi-engine book [pos]` (optional `--book <path>`), `koi-engine tb [pos]` (optional `--syzygy <path>`), `koi-engine see <move> [pos]`, `koi-engine eval [pos]` (`--eval [pos]`), `koi-engine options` (`--options`), `koi-engine perft <depth> [pos]` (`1..10`) — where `[pos]` accepts `startpos`, a quoted/unquoted FEN, or `[startpos|fen] [moves <uci>...]` — plus `koi-engine --hce` (force HCE without a network), `koi-engine --nnue <path>` (override `KOI_NNUE_PATH`/`KOI_EVALUATOR`), `koi-engine version` (`--version` / `-v`), `koi-engine --engine-json`, `koi-engine --protocol <uci|usi|ucci|ucicyclone|xboard>`, and `koi-engine help` (`--help` / `-h`).
- **Interactive commands (inside a UCI session):**
  - `d` — prints an ASCII board diagram of the current position, FEN string, Zobrist hash key, check status, legal move count, and FIDE game state (`ongoing`, `checkmate`, `stalemate`, or draw rule).
  - `fen` — prints the six-field FEN of the current position.
  - `key` — prints the hexadecimal 64-bit Zobrist position, pawn, and Polyglot book keys.
  - `moves` — lists all sorted legal moves in UCI coordinate notation.
  - `book` — lists weighted legal Polyglot opening book entries for the position.
  - `tb` — probes active Syzygy tablebases for root WDL outcome and best moves.
  - `eval` — prints the active evaluator architecture (`NNUE` container version/feature set/layer shape or `HCE`), static score (side-to-move and White perspectives), heuristic WDL triplet, and game phase / piece count.
  - `nnue` — prints the active loaded NNUE container metadata and verified payload SHA-256 checksum (or `NNUE : inactive (EvalMode=hce)` when running classical evaluation).
  - `flip` — toggles the side to move (when legal; rejects positions that would leave the non-moving king in check).
  - `makemove <uci...>` — plays one or more legal UCI moves transactionally onto the current board and history.
  - `undo [plies]` — steps back `N` plies (default `1`) in the current position's move history.
  - `history` — prints the position origin (`startpos` or FEN), ply count, move history, and full `position ...` command.
  - `see <move>` — prints the bitboard static exchange evaluation (SEE) of a legal UCI move in centipawns.
  - `compiler` — prints build version, commit hash, compiler toolchain, C++ standard, and compiled ISA (`AVX-512` / `AVX2` / `NEON` / `PEXT`).
  - `bench [depth [hash [threads]]]` — runs the deterministic benchmark (defaults: depth 10, 16 MB hash, 1 thread) and prints `Total time (ms)`, `Nodes searched`, and `Nodes/second`.
  - `speedtest [threads [hash [seconds]]]` — runs a timed multi-position NPS throughput benchmark (defaults: 1 thread, 128 MB hash, 3 seconds).
  - `options` — prints a snapshot of the live configured protocol, evaluator, search, book, and Syzygy option values.
  - `perft <depth>` / `go perft <depth>` — runs an interruptible perft divide (`depth 1..10`) from the current position.
  - `wait` — blocks until any finite active search or `perft` completes before executing the next command.
  - `help` — prints a concise interactive command reference.

## UCI smoke test

Run an end-to-end handshake, search, and clean shutdown after building:

=== "One-shot CLI (--cmd)"

    ```bash
    ./build/release/koi-engine -c "uci" -c "isready" -c "position startpos" -c "go depth 2"
    ```

=== "PowerShell"

    ```powershell
    $engine = (Resolve-Path .\build\release\koi-engine.exe).Path
    @(
        'uci'
        'isready'
        'position startpos'
        'go depth 2'
        'wait'
        'quit'
    ) | & $engine
    ```

=== "Linux / macOS (Bash)"

    ```bash
    printf 'uci\nisready\nposition startpos\ngo depth 2\nwait\nquit\n' | ./build/release/koi-engine
    ```

The output should include, in order, the versioned engine identification lines
(`id name Koi <version>` plus an `info string build <commit>` line), the
`RandomSeed`, `Hash`, `Threads`, `UCI_AnalyseMode`, `MultiPV`, `Ponder`,
`OwnBook`, `BookFile`, `BookDepth`, `BookRandom`, and `Clear Hash` option
declarations, `uciok`, `readyok`,
zero or more valid `info` lines, and one legal coordinate-notation `bestmove`
from the starting position (for example, `bestmove e2e4`).

## Supported UCI behavior

- `uci` reports the engine identity, the compatibility `RandomSeed` option,
  `Hash` (default 512 MB, range 1–4096 MB), `Threads` (default 1, capped at
  `min(64, hardware_concurrency)`), the opening book options, and the
  `Clear Hash` button. The `Speed` option was removed: time allocation is
  always the neutral 100 % factor (use `Slow Mover` for a coarse time tweak).
- `isready` responds immediately with `readyok`, including while searching.
- `ucinewgame` resets the position and clears the persistent search hash
  (equivalent to `Clear Hash`) after cancelling and joining any active search;
  the reset is recorded in the Debug log. `position startpos` and
  `position fen ...` set a position, optionally followed by legal UCI moves.
  Replacing the root cancels and joins the old search without leaking its
  result.
- `setoption name RandomSeed value 0` (the default) keeps book selection
  deterministic per position. A nonzero seed selects a different, fully
  repeatable stream for every position; normal `go` search itself is
  deterministic and never uses the seed.
- `setoption name Hash value <MB>` resizes the persistent search hash, and
  `setoption name Clear Hash` clears it. Either command stops and joins an
  active search before changing the table.
- `setoption name Threads value <N>` selects the worker count. `Threads 1`
  keeps the deterministic serial search. `Threads > 1` adds Lazy SMP helpers
  and is intentionally nondeterministic because helpers share the transposition
  table. Changing the thread option stops and joins the active search before the
  new snapshot is used by the next `go` command.
- When boot-time auto-discovery actually loaded a network, the `EvalFile` option
  advertises the informational default `<autodiscovery>`; echoing it back is a
  no-op that keeps the boot evaluator. The marker is not a file path.
- `setoption name EvalMode value hce` selects the clean hand-crafted evaluator
  for later searches; `value nnue` restores the current `EvalFile` or boot
  network. Switching stops and suppresses the search that is already running.
  `KOI_EVALUATOR=hce` selects the HCE at boot without needing a network.
- `setoption name EvalFile value <path>` loads a Koi NNUE network for the next
  search (an empty value keeps the boot-time evaluator, so replaying defaults
  never triggers a load). A rejected or missing file leaves the current
  evaluator in place and reports `info string evaluator nnue` and `info string NNUE enabled from <path>` on success,
  or `info string EvalFile rejected: ...`. Running
  searches keep the evaluator they started with.
- `setoption name BookRandom value false` (the default) selects the highest-
  weight legal Polyglot move, using deterministic coordinate ordering for equal
  weights. `BookRandom true` enables weighted random selection; the default
  `RandomSeed 0` always picks the same move for a given position, while a
  nonzero seed draws a repeatable but different sequence.
- `setoption name BookSafety value true` (the default) runs a shallow forcing
  material probe before accepting a book move. A move that immediately hangs a
  valuable piece is rejected and normal search chooses the move. Set
  `BookSafetyDepth` from `0` through `3` to control the probe horizon; `0`
  disables the probe while retaining legal-move filtering. Safety never
  overrides analysis, MultiPV, ponder, infinite, or `searchmoves` book bypass.
- Book files larger than 16 MiB, malformed files, missing files, and load or
  allocation failures are treated as unusable so normal search can continue.
- `go` accepts `depth`, `nodes`, `mate`, `perft`, `movetime`, `wtime`, `btime`,
  `winc`, `binc`, `movestogo`, and `infinite`. Malformed limit values are
  ignored. `go mate N` searches to the depth that proves or refutes a mate in
  `N` (2N-1 plies). `go perft N` answers with `info string <move>: <nodes>`
  lines plus `info string Nodes searched: <total>` and no `bestmove`. A bare `go`
  uses a 250 ms move-time fallback. If a clock is supplied
  only for the non-moving side, Koi uses the same bounded fallback so malformed
  or asymmetric GUI commands cannot leave the engine searching indefinitely.
- A depth limit is capped internally at 64 plies. `nodes`, `movetime`, and
  side-to-move clock limits stop search at their requested boundary; `infinite`
  continues until `stop`. `depth` and `nodes` are upper bounds: when a clock or
  `movetime` is also supplied, whichever limit is reached first stops the search,
  so `go depth 6 wtime 200` cannot overrun the clock.
- Search reports completed iterations as UCI `info depth ... score ... nodes
  ... nps ... hashfull ... time ... pv ...` lines, where `hashfull` is the
  approximate transposition-table occupancy in permill (0..1000). An iteration
  whose root verification stayed selective republishes the most recent proven
  score at the current depth while keeping its own principal variation. Before
  any proven score exists, the line publishes its estimate unmarked: UCI has no
  "estimate" marker, and omitting the score makes analysis GUIs display 0 cp.
- During a long search iteration the engine may publish the in-flight depth
  and principal variation with refreshed `nodes`, `nps`, and `time` values
  (interim progress lines), so a GUI readout does not freeze on the last
  completed depth; the published score and bound continue to describe the
  most recent completed iteration.
- `stop` cancels and joins the active worker and emits exactly one final legal
  `bestmove` for that search. The final `bestmove` includes a
  ` ponder <reply>` suffix whenever the completed principal variation contains
  a reply, independent of the `Ponder` option.
- A terminal position with no legal moves returns `bestmove 0000`.
- `quit` and input EOF cancel and join the worker without late protocol output.

### UCI Options Quick Reference

Koi advertises 27 options during the `uci` handshake (plus two unadvertised `Debug` / `DebugFile` diagnostics options):

| Option | Type | Default | Range / Values | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| `RandomSeed` | `spin` | `0` | `0..2147483647` | Seed for `BookRandom` Polyglot selection (`0` = deterministic) |
| `Hash` | `spin` | `512` | `1..4096` MB | Transposition table size |
| `Threads` | `spin` | `1` | `1..min(64, cores)` | Search threads (`1` = deterministic; `> 1` = Lazy SMP) |
| `UCI_AnalyseMode` | `check` | `false` | `true` / `false` | Analysis mode (bypasses opening book and Syzygy root moves) |
| `MultiPV` | `spin` | `1` | `1..16` | Number of principal variations to search and emit |
| `Ponder` | `check` | `false` | `true` / `false` | Enable GUI `go ponder` / `ponderhit` workflow |
| `OwnBook` | `check` | `false` | `true` / `false` | Use Polyglot opening book at the root |
| `BookFile` | `string` | `book.bin` | Path | Polyglot `.bin` opening book path (auto-detected if present) |
| `BookDepth` | `spin` | `16` | `0..40` | Maximum fullmove plies to play from book (`0` = unlimited) |
| `BookRandom` | `check` | `false` | `true` / `false` | Weighted random book choice vs highest-weight move |
| `BookSafety` | `check` | `true` | `true` / `false` | Reject book moves that hang material in a shallow tactical check |
| `BookSafetyDepth` | `spin` | `2` | `0..3` | Tactical safety check horizon for `BookSafety` |
| `Clear Hash` | `button` | — | — | Immediately zeroes the transposition table |
| `UCI_ShowWDL` | `check` | `false` | `true` / `false` | Append `wdl W D L` permill probabilities to proven `info` lines |
| `Move Overhead` | `spin` | `30` | `0..5000` ms | Per-move GUI/network latency reserve subtracted from clocks |
| `Dynamic Overhead` | `check` | `false` | `true` / `false` | Learn additional network round-trip latency from clock deltas |
| `Slow Mover` | `spin` | `100` | `10..1000` % | Time usage percentage multiplier for clocked searches |
| `Contempt` | `spin` | `0` | `0..1000` cp | Dynamic contempt bonus scaled against symmetric draw scores |
| `StrengthMode` | `check` | `false` | `true` / `false` | Accepted compatibility flag (neutral at full strength) |
| `SyzygyPath` | `string` | `""` | Path list | Syzygy tablebase directory/path list (or auto-detected `syzygy/`) |
| `SyzygyProbeDepth` | `spin` | `1` | `1..100` | Minimum depth for root Syzygy probes |
| `SyzygyProbeLimit` | `spin` | `7` | `0..7` | Maximum piece count eligible for Syzygy probing |
| `SyzygyInteriorDepth` | `spin` | `0` | `0..100` | Maximum remaining depth for interior WDL cutoffs (`0` = off) |
| `Syzygy50MoveRule` | `check` | `true` | `true` / `false` | Respect 50-move draw clock in Syzygy probing |
| `EvalFile` | `string` | `""` | Path | NNUE network path (`<autodiscovery>` marker when auto-loaded) |
| `EvalMode` | `combo` | `nnue` | `nnue`, `hce` | Active evaluator backend (NNUE or clean tapered HCE) |
| `Protocol` | `combo` | `uci` | `uci`, `usi`, `ucci`, `ucicyclone`, `xboard` | Wire protocol dialect (or `--protocol <name>` at launch) |

### Timing controls

Koi separates search limits from time-allocation policy. Explicit `go depth`
and `go nodes` searches without a clock or `movetime` are not given an
artificial time limit, and `go infinite` always searches until `stop`. When a
depth or node limit is combined with a side-to-move clock or `movetime`, the
explicit limit stays a strict upper bound while the time limit remains a
deadline: the search stops at whichever is reached first.
For clock searches, the requested budget is adjusted in this order:
`Slow Mover`, then `Move Overhead` is subtracted, followed by the existing
safety margin and minimum safe budget. An explicit `movetime` is a direct
request (only the safety margin applies; `Move Overhead` is not subtracted) and
the budget never exceeds the requested value; `Slow Mover` does not extend a
`movetime` search.
`Move Overhead` defaults to 30 ms and accepts 0..5000; `Slow Mover` defaults to
100 and accepts 10..1000. These controls affect allocation, not explicit depth
or node limits, and changing one while searching cancels and joins the old
generation before the next search uses the new snapshot.

For online bots and network GUIs, Koi also learns the GUI round trip: when a
clock search reports less remaining time than Koi's own elapsed search plus the
reported increment explains (excess ≥ 25 ms), the learned overhead rises up to
500 ms and is added to `Move Overhead` for reserve and hard-safety purposes; a
clean or faster clock decays it. This makes repeated small network stalls
self-correcting without slowing `movetime` analysis searches. `ucinewgame`
resets the estimate. If a bot's link is consistently slower than 500 ms, set
`Move Overhead` explicitly (for example 800–1500 ms for a high-latency server)
and/or enable `Dynamic Overhead`.

Clock searches are additionally flag-proof. A single move can never consume more
than a quarter of the usable clock, the hard allocation is two base budget units
(not three, even with an increment), the increment credit is capped at that same
quarter, and a final ceiling reserves `max(2 x Move Overhead + 15 ms, min(50 ms,
remaining/20))` on top of the normal reserve. The stop deadline is fixed when the
search starts, so iteration evidence can pace the search but can never push it
past the safe ceiling. `ponderhit` converts a still-running ponder search in
place: the same worker keeps its accumulated work and switches to the original
limit or clock budget with the ponder flag cleared. If no ponder search is
running (for example a node-limited ponder already completed, or `ponderhit` is
sent while idle), the command is ignored and no extra search starts. `time_manager_tests` and
`koi_engine_time_safety_process` enforce these invariants.

### WDL and strength controls

`UCI_ShowWDL` defaults to false. When enabled, proven alpha-beta `info` lines
append a deterministic `wdl W D L` triplet. A selective iteration without a
proven score direction still publishes its best available estimate `score`
unmarked (so analysis GUIs never drop to 0 cp) while omitting `wdl`. Koi does not advertise `UCI_LimitStrength` or `UCI_Elo`:
the previous node-cap mapping was not calibrated to an actual rating. A GUI
that sends those legacy options receives normal full-strength play.
Stockfish's separate Elo controls remain available to the measurement tools
when Stockfish is the opponent.

### Optional Syzygy tablebases

Syzygy support is optional and never requires tablebase files for build, startup,
or ordinary search. Set `SyzygyPath` to a directory containing licensed `.rtbw`
and `.rtbz` files, or drop them into a conventional `syzygy/` directory beside
the executable and let automatic detection find them. `SyzygyProbeLimit` accepts 0..7 pieces (default 7),
`SyzygyProbeDepth` accepts 1..100 (default 1) and is a minimum depth: the root
probe is used once the search reaches that depth, and `Syzygy50MoveRule`
defaults to true. Root wins are reported with a decisive centipawn score and an
exact `wdl` triplet when `UCI_ShowWDL` is enabled; long tablebase wins are never
advertised as `mate 1`. An empty, missing, unreadable, malformed, over-limit, or
unsupported position safely falls back to normal search. Root WDL/DTZ selection
is used only for eligible single-PV play searches; analysis mode, `MultiPV > 1`,
ponder, `go infinite`, and `searchmoves` retain their documented search paths.
Successful probes may be reported as `tbhits` in valid `info` lines. No
tablebase data is distributed with Koi.

**Automatic detection.** If you place the tables in a directory named `syzygy`
(or `syzygy345`, `tablebases`, `3-4-5`) next to the engine executable, Koi uses
them automatically; you do not have to set `SyzygyPath`. The load happens at the
first `isready`/`go`, and the engine reports
`info string syzygy detected: <path> (SyzygyPath not set, using it)`. Setting
`SyzygyPath` explicitly — even to an empty value — turns automatic detection off
for that session. Tables remain user-installed and are never bundled in release
archives.

`SyzygyInteriorDepth` accepts 0..100 (default 0, which disables interior
probing entirely). When set above zero, search nodes at or below that remaining
depth may also take a decisive WDL cutoff, under the same eligibility rules as
the root probe. Only decisive results apply: a win or loss is adopted, while
draw, cursed-win, and blessed-loss results never override search, and a cutoff
still requires a zero halfmove clock when `Syzygy50MoveRule` is true. Interior
probing is experimental and strength-unvalidated (no tablebase assets were
available to test it against), so leave the option at its default 0 unless you
are deliberately experimenting. The option is inert unless tables are available
(explicit `SyzygyPath` or automatic discovery)
or a test probe hook is installed. The `SyzygyProbeDepth` gate on the root probe
is unchanged by this option.

### Hidden developer diagnostics

The unadvertised `Debug` check option and `DebugFile` string option are for local
diagnostics only. `Debug` defaults to false. The same switch can be toggled
during a session with the UCI `debug on` and `debug off` commands; changing it
stops and joins any active search, exactly like the option. With an empty
`DebugFile`, Koi writes
`koi-debug.log` beside the executable; a relative path is also resolved beside
the executable, while an absolute path is used as supplied. Logs are best-effort,
rotate at 8 MiB, and retain three backups. Debug events never go to UCI stdout
or normal stderr, so a valid En Croissant or other UCI transcript remains
protocol clean. Leave this option disabled for normal release use.

## Register in En Croissant (primary)

1. Build the Release target and resolve the absolute path to
   `build\release\koi-engine.exe` (`build/release/koi-engine` on Linux and macOS) — or
   the executable in your chosen build directory.
2. In **Pawn Appétit** / **En Croissant**, add a UCI engine and select that `koi-engine` path.
   Keep the engine's working directory beside the executable when configuring
   the engine so portable relative assets resolve predictably.

    !!! tip "Showing a separate `VERSION` badge (like Stockfish)"
        Pawn Appétit / En Croissant's local binary scanner only populates `Name` from `id name` and leaves `Version` blank. Run `koi-engine --engine-json` (optionally with `--threads <n> --hash <mb>`, e.g. `koi-engine --threads 4 --hash 256 --engine-json`) to print a ready-to-paste JSON profile (`name: "Koi"`, `version: "<semver>"`, resolved `path`, defaults) for the GUI's **Edit JSON** dialog (or inspect `koi-engine --version`).

   - **Protocols (`uci`, `usi`, `ucci`, `ucicyclone`, `xboard`):** Select `Protocol` in UCI options (or launch with `--protocol <name>`, or send `xboard` / `usi` / `ucci` directly on stdin). All modes target standard 8x8 chess (`xboard` implements CECP v2 `protover 2`, `feature`, `setboard`, `usermove`, `ping/pong`, `level`/`st`/`sd`, `time`/`otim`, `post`/`nopost`, `analyze`/`exit`, `undo`/`remove`; `usi` and `ucci`/`ucicyclone` support their handshake, option, `go`, `bestmove resign`/`nobestmove`, and `bye` conventions on 8x8 boards).
3. Release archives ship a compact Stockfish-verified CC0 opening book as
   `book.bin` beside `koi-engine.exe` (used automatically unless `OwnBook` is
   explicitly `false`); to use another Polyglot book, replace `book.bin` or set
   `BookFile` with `OwnBook=true`. Do not add unlicensed book data to this
   repository or redistribute it without its license.

Recommended starting options are:

```text
setoption name Hash value 512
setoption name Threads value 4
setoption name OwnBook value true
setoption name BookFile value book.bin
setoption name BookDepth value 16
setoption name BookRandom value false
setoption name BookSafety value true
setoption name BookSafetyDepth value 2
```

For normal play, let En Croissant provide the position and clock limits. For
analysis, enable `UCI_AnalyseMode` and use `go infinite`; send `stop` when the
analysis view is closed. Tutor and MultiPV views should set `MultiPV` to the
number of variations requested (for example, `3`) and use a finite depth or
clock search. Analysis mode, `MultiPV > 1`, `go infinite`, ponder, and
`searchmoves` intentionally bypass the opening book so these views receive
search variations rather than a book move.

En Croissant uses the standard UCI protocol: the checked-in process transcript
covers the handshake, options, positions with moves, stopped searches,
MultiPV, infinite analysis, and clean quit with exactly one legal `bestmove`
per search. Any other standard UCI GUI can use the same executable and options.

## Generic UCI fallback (including Lucas Chess)

1. Build the engine and resolve the path to `koi-engine` (`koi-engine.exe` on Windows).
2. In Lucas Chess, open the engine-management or configuration dialog and add
   the built executable as an external UCI engine.
3. Save the engine configuration, then select Koi for play.

Recommended starting settings for a machine with sufficient memory are:

```text
setoption name Hash value 512
setoption name Threads value 4
```

The hash is one shared total table and is not multiplied by the thread count.
Reduce `Hash` or `Threads` if other applications need the memory or CPU.

### Lucas Chess workflows

Koi's `OwnBook` option defaults to false, but a Polyglot book placed beside the
executable is auto-detected and used unless `OwnBook` is set explicitly; release
packages bundle the compact Stockfish-verified CC0 book flat beside the binary
as `book.bin` so auto-detection works out of the box.
A relative `BookFile` resolves from the engine's executable directory, not Lucas
Chess's working directory. To force a specific book, set `OwnBook=true` with:

```text
setoption name OwnBook value true
setoption name BookFile value book.bin
setoption name BookDepth value 16
setoption name BookRandom value false
setoption name BookSafety value true
setoption name BookSafetyDepth value 2
```

`BookDepth 0` leaves the book unlimited; values from `1` through `40` limit
the exclusive root ply depth. With `BookRandom false`, Koi chooses the
highest-weight legal move deterministically; equal weights use coordinate
ordering. Set `BookRandom true` only when weighted variety is wanted. On a hit Koi writes
`info string book move <uci> depth <ply>` followed by that one legal
`bestmove`. A missing or invalid book silently falls back to search.

For normal play, set the position supplied by Lucas Chess and use its clock
limits, for example `go wtime 60000 btime 60000`. Koi returns one final legal
`bestmove` for that search.

For continuous analysis, enable analysis mode and start an infinite search:

```text
setoption name UCI_AnalyseMode value true
go infinite
```

Send `stop` when analysis is no longer needed. `go infinite` is analysis mode:
it continues until stopped rather than completing from a time or depth limit.

For tutor-style analysis, request multiple principal variations:

```text
setoption name MultiPV value 3
go depth 12
```

Koi emits one `info` line per principal variation, with `multipv 1` as the
best-ranked line. Use `go ... searchmoves e2e4` (with any legal coordinate
moves required) to restrict the legal root moves considered by that search.
Opening-book selection is intentionally disabled for `UCI_AnalyseMode`, `MultiPV`
values greater than one, `go infinite`, `go ponder`, and any `go ... searchmoves ...`
command, so those Lucas Chess tutor and analysis workflows always use search results.

For ponder support, enable it before Lucas Chess begins pondering:

```text
setoption name Ponder value true
go ponder wtime 60000 btime 60000
ponderhit
```

When the expected move arrives, `ponderhit` keeps the running ponder search and
converts it in place to a normal timed search, so no speculative work is
discarded. If no ponder search is running, an idle `ponderhit` is ignored (no
search starts). Send `stop` if the expected move
did not arrive or the GUI cancels the ponder search.

Menu labels can vary by Lucas Chess version. Use an absolute executable path,
or another path that remains valid when Lucas Chess starts the engine.

Before registering it, check the process transcript: stdout should contain
only valid UCI responses, with no logging or diagnostics mixed into it. For
the smoke test, also confirm that stderr is empty for a valid transcript. If
Lucas Chess cannot start the engine, verify the executable path, that the
Release build exists, and that the process can complete the `uci` / `isready`
handshake from your terminal.

For final Lucas acceptance, play at least one short standard game after the
handshake succeeds. Confirm that the GUI receives a legal move after `go`,
remains responsive while the engine is thinking, and can stop or start a new
game without a duplicate `bestmove`. This repository automates the UCI process
transcript but cannot automate a locally installed Lucas Chess GUI.

## Configuration and release packaging

The Koi v1.2.0 Windows package is `koi-engine-v1.2.0.zip`; the portable
Linux archive is
`koi-engine-v1.2.0-linux-x86_64.tar.gz`; macOS arm64 (Apple Silicon) archives
are published alongside them. Each user-facing archive is intentionally minimal
and contains only the platform engine executables (`koi-engine` plus AVX2/AVX-512
siblings on Windows/Linux), the bundled `koi.nnue`, the bundled Stockfish-verified
CC0 opening book (`book.bin`), and `LICENSE`. Standalone `koi.nnue` is attached alongside the platform archives on GitHub Releases (without `.package.json` or `.sha256` clutter in the Releases Assets tab; GitHub displays built-in SHA-256 digests and SLSA build-provenance attestations).

Koi has no required configuration file. En Croissant or another UCI GUI sends
the options at session start; the portable release defaults are `RandomSeed=0`,
`Hash=512`, `Threads=1`, `UCI_AnalyseMode=false`, `MultiPV=1`,
`Ponder=false`, `OwnBook=false`, `BookFile=book.bin`, `BookDepth=16`,
`Contempt=0`, `Dynamic Overhead=false`,
`BookRandom=false`, `BookSafety=true`, `BookSafetyDepth=2`,
`UCI_ShowWDL=false`, `Move Overhead=30`, `Slow Mover=100`, `StrengthMode=false`,
`SyzygyPath=""`, `SyzygyProbeDepth=1`, `SyzygyProbeLimit=7`,
`Syzygy50MoveRule=true`, `SyzygyInteriorDepth=0`, `EvalFile=""`
(empty keeps the boot-time evaluator), `EvalMode=nnue`, and `Protocol=uci`.
For the recommended En Croissant smoke scenario, use `Hash=512` and `Threads=4`,
then keep the book and Syzygy paths explicitly configured if those
assets are available.

Release archives ship the compact Stockfish-verified CC0 `book.bin` (built by
`tools/book/build_verified_book.py`) alongside the engine binaries; users may
still replace `book.bin`, pass a custom `BookFile`, or use
`tools/build/install_book.ps1`. Never redistribute a book whose license does not
permit it.

Release archives contain strictly the engine binaries, `koi.nnue`, `book.bin`,
and `LICENSE`.

!!! note "Source-only developer tools"
    Developer tools (`koi-bench`, `koi-replay`, `koi-perft`, `koi-eval-features`, `koi-nnue-fixture`) and the test suite are excluded from user-facing release archives; build them from source via `CMakePresets.json` when needed.
The repository retains third-party license texts under `third_party/` (`chess-library` and `fathom`), while generated release archives keep the flat layout above.
Do not include generated benchmark profiles, match JSON/PGN, debug logs, or
tablebase data in the repository release commit; write those artifacts under the
repository-local artifacts directory such as `artifacts/verification` or
`artifacts/matches`. An Elo, CPL, or strength
claim requires fresh comparable Stockfish CPL or match data with the executable,
options, time control, and input provenance recorded alongside the report.

## References

- [CLI & Interactive Reference](CLI.md)
- [En Croissant](https://encroissant.org/)
- [Lucas Chess](https://lucaschess.pythonanywhere.com/)
- [Universal Chess Interface (UCI) reference](https://www.shredderchess.com/chess-info/features/uci-universal-chess-interface.html)
- [Disservin/chess-library](https://github.com/Disservin/chess-library)
- [Fathom (Syzygy probing library)](https://github.com/jdart1/Fathom)
- [C++ compiler support for C++26](https://en.cppreference.com/w/cpp/compiler_support/26)
