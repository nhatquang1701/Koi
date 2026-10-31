<h1 align="center">Koi</h1>

<p align="center">
  A free, open-source UCI chess engine for standard chess.<br>
  <a href="https://nhatquang1701.github.io/Koi/"><strong>Explore the documentation »</strong></a>
</p>

<p align="center">
  <a href="docs/USER_GUIDE.md#supported-uci-behavior"><img src="https://img.shields.io/badge/PROTOCOL-UCI-2e9d59?style=for-the-badge" alt="Protocol: UCI"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/LICENSE-MIT-2e9d59?style=for-the-badge" alt="License: MIT"></a>
  <img src="https://img.shields.io/badge/LANGUAGE-C%2B%2B26-2e9d59?style=for-the-badge" alt="Language: C++26">
  <img src="https://img.shields.io/badge/PLATFORMS-Windows%20x64%20%7C%20Linux%20x86_64%20%7C%20macOS%20arm64-2e9d59?style=for-the-badge" alt="Platforms: Windows x64, Linux x86-64, and macOS arm64">
</p>

<p align="center">
  <a href="https://github.com/nhatquang1701/Koi/issues/new">Report an issue</a>
  ·
  <a href="https://github.com/nhatquang1701/Koi/releases">Releases</a>
</p>

## About Koi

Koi is a C++26 chess engine for Windows x64, Linux x86-64, and macOS arm64
(Apple Silicon). Its default search
uses iterative-deepening alpha-beta with a persistent transposition table.
Release packages ship the gated Koi-native NNUE evaluator as the primary evaluator
(bundled at `nnue/koi.nnue`, auto-discovered at boot). The hand-crafted classical
evaluator was removed before v1.1.5 (a clean rewrite is planned), so a network is
required to search: a boot without a usable network exits with a clear diagnostic.

| Engine area | What Koi provides |
| --- | --- |
| Search | Alpha-beta with Lazy SMP |
| Evaluation | Gated Koi NNUE required in release packages (bundled); classical HCE removed 2026-10-28 (clean rewrite planned); experimental GPU NNUE off by default |
| Chess support | Standard chess over the UCI protocol |
| Optional assets | Bundled CC0 Polyglot opening book, used automatically; user-installed Syzygy tablebases are auto-detected from a conventional directory beside the engine and are never packaged |

## Name

The engine is called **Koi**. Releases up to and including v1.1.5, their package
metadata and archived documentation used the name "Koi Engine"; those historical
artifacts are left unchanged. New releases, the UCI identity and the current
documentation use **Koi**. Executable and archive file names keep the
`koi-engine` spelling for tooling compatibility.
## Download

Get the Windows, Linux x86-64, and macOS arm64 packages and their checksum
sidecars from
[GitHub Releases](https://github.com/nhatquang1701/Koi/releases).

- Windows: `koi-engine-v1.1.5.zip`
- Linux x86-64: `koi-engine-v1.1.5-linux-x86_64.tar.gz`
- macOS arm64: `koi-engine-v1.1.5-macos-arm64.tar.gz`

macOS arm64 (Apple Silicon) can also build from source: install Homebrew LLVM
(clang 19+; AppleClang 18+ also works), CMake, and Ninja, then configure with
`-DKOI_BUILD_MODULES=OFF -DKOI_STATIC_RUNTIME=OFF`. GPU NNUE is experimental,
disabled by default, CUDA-only, and
unavailable on macOS; the CPU NNUE path works normally. NNUE is required: a boot
without a usable network exits with a clear diagnostic.
See the [user guide](docs/USER_GUIDE.md#build-prerequisites) for the commands.

## Get started

1. Download and extract the archive for your system (or build from source when
   no package matches your platform).
2. Add `koi-engine.exe` (Windows) or `koi-engine` (Linux and macOS) as a UCI
   engine in En Croissant or another compatible chess GUI.
3. Start with `Hash=512` and `Threads=1`.

Koi needs no configuration file or extra assets. `Threads=1` gives deterministic
search; multiple threads use Lazy SMP and can produce different valid
principal variations. `OwnBook` defaults to false, but a Polyglot book beside
the executable is auto-detected and used (release packages bundle a custom CC0
book). The gated NNUE network is bundled too and is required at boot; a missing
or rejected network is a startup error (exit 4), while a runtime `EvalFile`
rejection keeps the current evaluator and reports an `info string`.

## Explore

- [Documentation site](https://nhatquang1701.github.io/Koi/) — installation,
  UCI options, GUI setup, development guidance, and project archive.
- [User guide](docs/USER_GUIDE.md) — build instructions, protocol behavior,
  optional assets, and tools.
- [Documentation map](docs/README.md) — maintained documentation and history.

## License

Koi is released under the [MIT License](LICENSE). Release packages also
include the required licenses for vendored components.
