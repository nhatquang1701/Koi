# Apple Silicon (`macOS arm64`) Worker Setup

How to build, test, and benchmark Koi on an Apple Silicon Mac—the quiet reference
platform used for Koi's speed gates, depth sweeps, NNUE training, and equal-time
SPRT matches.

## Toolchain

- macOS 14+ on Apple Silicon.
- Xcode command line tools: `xcode-select --install` (AppleClang 18+ works).
- Homebrew LLVM is optional; the documented configuration uses the default
  AppleClang with C++26 modules disabled.
- CMake 3.31+ and Ninja: `brew install cmake ninja`.

## Build

=== "CMake Preset"

    ```bash
    cmake --preset macos-arm64-release
    cmake --build --preset macos-arm64-release -j 6
    ```

=== "Explicit CMake"

    ```bash
    cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DKOI_BUILD_MODULES=OFF -DKOI_STATIC_RUNTIME=OFF \
      -DPython3_FIND_FRAMEWORK=LAST
    cmake --build build/release -j 6
    ```

=== "Makefile (OpenBench / PGO)"

    ```bash
    make release
    make profile-build EVALFILE=artifacts/networks/koi-v9-hm10-h256-official.nnue
    make smoke
    ```

!!! note "macOS build flags"
    - `KOI_BUILD_MODULES=OFF` — AppleClang cannot build the C++26 named modules.
    - `KOI_STATIC_RUNTIME=OFF` — the static runtime policy is Linux-only.
    - `-DPython3_FIND_FRAMEWORK=LAST` avoids picking the system Python framework.
    - After changing the compiler or flags, delete `build/release` before reconfiguring: CMake caches the compiler choice.

## Test

```bash
ctest --test-dir build/release -j 8 --output-on-failure
```

## Measuring (the speed gates)

- Fixed-depth node/time profile:

  ```bash
  build/release/koi-bench --depth 6 --threads 1 --timed --fen-file <fens.txt>
  ```

- Depth sweep with per-depth rows plus JSON reports:

  ```bash
  build/release/koi-bench --depth-sweep 1..8 --threads 1 --timed \
    --fen-file <fens.txt> --report out.json --profile-json profile.json
  ```

  `--warmup` and `--repeat` default to 0/1; `--depth` and `--depth-sweep` are
  mutually exclusive. The fen-file line format is `[name|]<FEN or move list>`.

- Self-play A/B (node-limited, engine vs engine): `tools/stability/uci_match.ps1`
  with `-KoiPath` / `-OpponentPath`; install PowerShell 7 on macOS with
  `brew install --cask powershell`.

- Reference throughput (Apple Silicon M4, `Threads=1`, Official NNUE v9 `h256` with PGO+LTO): ~3.4–4.2M NPS in `1+0` self-play and ~12–14M NPS at `Threads=4`. Use `build/release/koi-engine bench` for a quick single-threaded deterministic node-count and NPS check.

## Remote worker pattern

The repository's measurement scripts drive the Mac over SSH:

1. `scp` the script and its inputs, then start it detached with a log:

   ```bash
   ssh <host> "cd ~/koi-run && nohup bash <script> > <script>.log 2>&1 &"
   ```

2. The script echoes a marker line (for example `JOB-DONE`) as its last
   action.
3. A watcher on the driving side polls for the marker, then `scp`s the results
   back. Keep poll windows generous — long sweeps outlive short ones.

!!! warning "Remote worker gotchas"
    - Non-login SSH shells do not see Homebrew on `PATH`; use the default compiler or absolute paths in scripts.
    - macOS has no GNU `timeout`; avoid it in scripts.
    - Run benches and builds on separate cores when measuring: scheduling a bench onto an efficiency core slows it noticeably.
    - `koi-bench` buffers stdout to a file until exit; use the JSON reports or the marker line to detect completion rather than reading partial logs.
