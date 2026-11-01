# Adversarial UCI move-list fuzz — Koi (2026-11-01)

## Purpose
The deterministic corpus has a small number of move lists. This fuzzer feeds
generated `position startpos moves ...` sequences through a real UCI session:
valid random games, mutated games, random UCI-like tokens (illegal squares,
wrong promotions, Chess960-style castling `e1h1`, control bytes), and very long
token lists (up to ~1500 tokens).

Script: `tests/python/reliability/move_fuzz.py`
(deterministic: `random.Random(20261101 + case)`; committed fixtures (generated once with python-chess) supply legal
random games; malformed tokens are generated from a constrained alphabet).

Per case the engine receives:
```
uci
isready
position startpos moves <generated>
go depth 1
stop
quit
```
Invariants: exit 0, no timeout, stderr only `UCI position error:` lines, exactly
one grammatical bestmove (`0000` allowed) from the retained position.

## Results

| Run | Cases | Accepted move lists | Max tokens | Timeouts | Non-zero exits | Unexpected stderr | Bad stdout |
|---|---|---|---|---|---|---|---|
| Release MSVC | 2000 | 386 | 1493 | **0** | **0** | **0** | **0** |
| MSVC ASan | 500 | 86 | 1493 | **0** | **0** | **0** | **0** |

Commands:

```powershell
$env:KOI_NNUE_PATH = "<promoted net>"
python tests/python/reliability/move_fuzz.py `
  --engine build/full-release-gpu/koi-engine.exe --cases 2000 --workers 6 `
  --out artifacts/plans/reliability-2026-10-31/move-fuzz.json
# ASan: --engine build/asan-reliability/koi-engine.exe --cases 500 --workers 4
```

No engine change was required; the fuzzer is retained as a repeatable tool
alongside `fen_fuzz.py`.
