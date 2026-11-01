# Adversarial `setoption` fuzz — Koi (2026-11-01)

## Purpose
Exercises `UciController::handle_setoption` parsing and state handling with
random/hostile option commands, then requires one healthy depth-1 search.

Script: `artifacts/plans/reliability-2026-10-31/option_fuzz.py`
(deterministic: `random.Random(20261101 + case)`). Each session sends 5–40
random commands, each followed by `isready`, then `position startpos / go depth
1 / stop / quit`.

Generators:
- **valid option names** with hostile values: empty/quoted/backslash/Unicode/
  control-byte strings, 4 KB and 16 KB values, numeric extremes;
- **malformed syntax**: `setoption`, `setoption name`, missing `value`, mixed
  case, extra spaces;
- **unknown names** and long random names;
- `Hash` and `Threads` values are intentionally constrained to invalid/small
  values — the fuzzer must not allocate resources, only stress the parser.

Invariants: exit 0, no timeout, only `id`/`option`/`info`/`uciok`/`readyok`/
`bestmove` lines on stdout, stderr only `UCI position error:` (none expected),
and exactly one grammatical bestmove.

## Results

| Run | Cases | Timeouts | Non-zero exits | Unexpected stderr | Bad stdout |
|---|---|---|---|---|---|
| Release MSVC | 1000 | **0** | **0** | **0** | **0** |
| MSVC ASan | 300 | **0** | **0** | **0** | **0** |

Commands:

```powershell
$env:KOI_NNUE_PATH = "<promoted net>"
python artifacts/plans/reliability-2026-10-31/option_fuzz.py `
  --engine build/full-release-gpu/koi-engine.exe --cases 1000 --workers 6 `
  --out artifacts/plans/reliability-2026-10-31/option-fuzz.json
# ASan: --engine build/asan-reliability/koi-engine.exe --cases 300 --workers 4
```

No engine change was required. Together with `fen_fuzz.py` and `move_fuzz.py`
this covers the three protocol input parsers (FEN, move lists, options).
