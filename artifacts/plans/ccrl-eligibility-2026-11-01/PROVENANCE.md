# Provenance / "no copied code" audit — Koi (2026-11-01)

Scope: what is in the submitted engine, what third-party code exists, and how
CCRL's "coded on your own or AI assisted" / "credit where due" questions are
answered.

## Engine source

- All engine code lives in `src/` (namespace `koi`) and is Koi's own C++26
  implementation: position/rules, UCI controller, search, NNUE inference,
  tablebases, book, time management and support code.
- No third-party engine source files (Stockfish, Ethereal, Berserk, …) are
  present in `src/`. `git ls-files src` contains only Koi's files.
- Comments in the search code cite **Stockfish 19** (and other public
  chess-engine literature) where a well-known algorithm is implemented or
  calibrated, e.g. "Follow Stockfish 19's ordering", "the same direction as
  Stockfish's skip_quiet_moves()". These are provenance notes for public
  algorithmic ideas; they are not copied source files.
- Development is **AI-assisted** (DeepSeek coding agent), disclosed per CCRL's
  "coded on your own or AI assisted" question. All design/testing/release
  decisions and review remain with the human author.

## Vendored third-party components

| Component | Path | License | Role in the submitted engine |
|---|---|---|---|
| chess-library | `third_party/chess-library` | MIT | header-only board/format helper used by compatibility adapters; license shipped in `licenses/chess-library-MIT.txt` |
| Fathom | `third_party/fathom` | MIT | Syzygy probing; license shipped in `licenses/fathom-MIT.txt` |
| Stockfish 19 | `third_party/stockfish-19` | GPL-3.0 | **test opponent only** — a binary used in local match/game tests, never linked into Koi and never included in release packages |

- `tools/build/package_release.ps1` copies only the engine executables,
  `README.md`, `LICENSE`, `licenses/` (chess-library MIT + Fathom MIT), the
  gated `nnue/koi.nnue`, and (when supplied) the CC0 book at `book/koi.bin`.
  It never copies `third_party/stockfish-19`, the test suite or developer
  tools.
- `koi-engine.exe` contains **zero** occurrences of the byte string
  `stockfish` (checked directly), so the GPL test oracle is not embedded or
  linked into the distributed engine.
- Root project license: MIT (`LICENSE`).

## Engine identity / credit statement

Koi is an independent, MIT-licensed UCI engine implemented in C++26. It uses
public chess-search and NNUE techniques; where behavior deliberately follows a
well-known engine (chiefly Stockfish 19), the source comments credit that
influence. No engine source code was copied from another program. The only
third-party code in the repository is the two MIT libraries above (licensed)
and the GPL-3.0 Stockfish binary used solely as a local test opponent.

This is the position to state truthfully in the CCRL submission ("AI assisted",
open source, credit to public algorithms and third-party libraries).
