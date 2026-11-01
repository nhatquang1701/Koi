# Adversarial `go` / state-machine fuzz — Koi (2026-11-01)

## Purpose
Randomizes UCI search limits and interleaves lifecycle commands to stress the
controller state machine beyond the fixed corpus: mixed/contradictory limits
(`depth/nodes/mate/movetime/wtime/btime/winc/binc/movestogo/infinite/ponder/
searchmoves`), random valid positions, `isready`, `ucinewgame`, `stop`,
`ponderhit`, MultiPV/Ponder/AnalyseMode toggles.

Script: `artifacts/plans/reliability-2026-10-31/go_fuzz.py`
(deterministic: `random.Random(20261101 + case)`). Each session:
- `uci`, `isready`,
- 3–15 random commands,
- deterministic healthy tail: `position startpos / go depth 1 / stop / quit`.

Invariants: exit 0, no timeout, stdout only `id`/`option`/`info`/`uciok`/
`readyok`/`bestmove`, stderr only `UCI position error:` (none expected), at
least one grammatical bestmove, no crash.

## Results

| Run | Cases | Timeouts | Non-zero exits | Unexpected stderr | Bad stdout |
|---|---|---|---|---|---|
| Release MSVC | 1000 | **0** | **0** | **0** | **0** |
| MSVC ASan | 300 | **0** | **0** | **0** | **0** |

Commands:

```powershell
$env:KOI_NNUE_PATH = "<promoted net>"
python artifacts/plans/reliability-2026-10-31/go_fuzz.py `
  --engine build/full-release-gpu/koi-engine.exe --cases 1000 --workers 6 `
  --out artifacts/plans/reliability-2026-10-31/go-fuzz.json
# ASan: --engine build/asan-reliability/koi-engine.exe --cases 300 --workers 4
```

No engine change was required. This complements the deterministic fuzz corpus
(especially the lifecycle/option-churn sections) with randomized state-machine
interleavings.
