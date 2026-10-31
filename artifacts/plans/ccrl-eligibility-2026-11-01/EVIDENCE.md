# CCRL evidence — 1000 ultra-fast games + UCI hardening

Date: 2026-11-01. Engine sources: Koi `9336a38` (Syzygy ASan fix) plus test-only
changes; current HEAD at test time `615df6d`. Mac build hash
`563cdde6ccbdae2443461d6276e3c27c39678fe7ea1cefebb58dee0126326ea6`;
promoted NNUE net `3e75e6d84d7b4ac0b0d46574aeb714032ea4ffff1a639c9d541e9663545829da`.

## 1000-game crash / legality run (Mac arm64 — authoritative)

Command (bot paused between games, restored afterwards):

```
python3 ~/koi-ccrl-crash_1000.py \
  --engine ~/koi-head/build/release/koi-engine \
  --opponent ~/koi-run/sf19/stockfish/stockfish-macos-universal \
  --openings ~/koi-head/tests/data/openings/openings-curated-32.txt \
  --games 1000 --workers 6 --movetime 5 \
  --out ~/koi-ccrl-20261101/1000-games.json
```

Harness details:
- Two fresh UCI processes per game; Koi vs the local GPL Stockfish 19 test
  opponent; Koi's colour alternates every game; neutral curated openings.
- Koi settings: `Hash 256`, `Threads 1`, `OwnBook false`, `Ponder false`,
  `MultiPV 1`, `SyzygyPath` empty; 64-bit arm64 engine, promoted net.
- Every move is legality-checked by python-chess; every engine stdout line is
  checked against the UCI grammar; engine stderr must stay empty; each engine
  must exit cleanly after `quit`.

Result (`artifacts/plans/ccrl-eligibility-2026-11-01/mac/1000-games.json`):

| Metric | Value |
|---|---|
| Games | 1000 |
| Crashes | **0** |
| Timeouts | **0** |
| Illegal moves | **0** |
| Protocol violations | **0** |
| Opponent errors | 0 |
| Outcomes | 997 checkmate, 3 fivefold |
| Wall time | 90.23 s (6 workers, 5 ms/move) |
| Bot restore | verified (`BOT_RESTORED=yes`) |

Per-game records: `mac/1000-games.jsonl` (168 KB).

Supplementary: the same harness also completed on Windows x86-64 (MSVC,
fixture NNUE net, 1000 games, 0 failures, 291.9 s) before the Mac run was
requested; `1000-games.json`/`.jsonl` in this directory are that run.

## UCI hardening evidence (same engine family)

- Deterministic UCI fuzz/transcript corpus (`koi_protocol_fuzz_process`):
  26 sections, including CCRL profile (`Hash 256`, `Threads 1`, `OwnBook false`,
  `Ponder false`, clock `go wtime/btime/winc/binc/movestogo`), option-invalid
  matrix, book hot-swap, binary/CRLF stdin, broken pipes, exactly-once
  bestmove/lifecycle storms. Release and MSVC ASan legs pass.
- 100,000-round resource-churn soak: 100,001 bestmoves, 14,969 scored lines,
  1,329.2 s, bounded RSS, exit 0.
- Option churn, broken-pipe and asset fail-closed suites pass.
- ASan: reliability process legs clean; unit label 20/24 with only local
  `%TEMP%` sandbox denials; no sanitizer reports.
- `OwnBook=false` disables an already detected book (covered by fuzz 9i) — the
  CCRL "internal book off" requirement.
- Binary contains no "stockfish" byte string; no third-party engine is linked.

## Caveats

- 5 ms/move is an ultra-fast crash/legality test, not a CCRL time control. It
  exercises the same UCI/stop/quit and move-validation paths CCRL managers use.
- The engine is standard chess only (no Chess960/variants).
- This is a dev build after the 1.1.5 tag; a new version number requires the
  author's explicit version bump. The 1.1.5 release archives predate the
  reliability fixes.
