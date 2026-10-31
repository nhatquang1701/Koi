# CCRL submission template — Koi

Fill the two TODO fields (author name/nationality) before sending. Everything
else is ready to paste into the TalkChess PM/email to the CCRL tester.

## Message

```
Engine name: Koi
Version: <1.1.5 | newer approved build>
Estimated strength: ~1650-1750 Elo (moderate confidence). Measured 1729 Elo
(95% CI 1659-1795) on a 128-game 200 ms ladder on Apple Silicon vs Stockfish 19
UCI_Elo anchors 1600/1800; this is not a CCRL-calibrated estimate.
Author: <TODO name>
Nationality: <TODO nationality>
Open source: yes, MIT — https://github.com/nhatquang1701/Koi
Homepage: https://github.com/nhatquang1701/Koi
Coded by: AI assisted (DeepSeek coding agent); algorithms are public
chess-engine techniques credited in source comments; third-party components are
MIT chess-library and MIT Fathom; the GPL-3.0 Stockfish 19 binary is used only
as a local test opponent and is not linked or packaged.
Executables: 64-bit AVX2 (koi-engine-avx2.exe, plus baseline selector and
AVX-512 builds for Windows x86-64; macOS arm64 build also available).
Recommended settings: Hash=256, Threads=1, OwnBook=false, Ponder=false,
MultiPV=1, SyzygyPath empty (no tablebases). The NNUE net must sit at
nnue/koi.nnue beside the binaries (or be copied next to koi-engine.exe).
Internal book: none required; the packaged book (book/koi.bin) is optional and
is not auto-loaded from that location. OwnBook=false disables any book,
including a detected one.
```

## Test evidence to attach/link

- 1000 ultra-fast games, Koi vs Stockfish 19, 5 ms/move, Mac arm64:
  **0 crashes, 0 timeouts, 0 illegal moves, 0 protocol violations**,
  997 checkmates + 3 fivefold, 90.2 s (`EVIDENCE.md`).
- UCI reliability suite: deterministic fuzz/transcript corpus (26 sections),
  100,000-round resource-churn soak, option-churn/broken-pipe/asset tests,
  ASan leg clean apart from local `%TEMP%` sandbox limitations.
- 64-bit AVX2 executable; standard chess only (no variants, no Chess960).
- No learning features. No persistent state. Quit/EOF clean.

## Files to provide

- `koi-engine.exe` (selector), `koi-engine-avx2.exe`, `koi-engine-avx512.exe`
  (Windows x86-64), `koi-engine` (Linux/macOS builds as applicable).
- `nnue/koi.nnue` (required; SHA-256 recorded).
- `README.md`, `LICENSE`, `licenses/chess-library-MIT.txt`,
  `licenses/fathom-MIT.txt`.
- Do **not** include: test suite, developer tools, third_party/stockfish-19,
  book (unless asked).

## CCRL settings used for the Koi-side evidence

```
uci
setoption name Hash value 256
setoption name Threads value 1
setoption name OwnBook value false
setoption name Ponder value false
setoption name MultiPV value 1
setoption name SyzygyPath value
ucinewgame
isready
go wtime 1000 btime 1000 winc 10 binc 10 movestogo 20
```

The `go` limits may come from the CCRL manager in its own form
(wtime/btime/winc/binc/movestogo); the engine supports them all.
