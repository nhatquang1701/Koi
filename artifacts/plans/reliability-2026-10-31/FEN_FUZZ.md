# Adversarial FEN parser fuzz — Koi (2026-11-01)

## Purpose
The deterministic UCI corpus only contains a handful of FENs. This standalone
fuzzer feeds 2000 generated FEN-like lines (roughly half deliberately malformed,
half valid random legal positions) through a real UCI session and checks crash,
hang, stderr and bestmove invariants.

Script: `tests/python/reliability/fen_fuzz.py`
(deterministic: `random.Random(20261101 + case)`; committed fixtures (generated once with python-chess) supply valid
positions; invalid boards/fields are generated from
random piece strings, mutated startpos, random ASCII and invalid field values).

Per case the engine receives:
```
uci
isready
position startpos
position fen <generated>
go depth 1
stop
quit
```
Expected: exit 0, no timeout, stderr only `UCI position error:` lines, exactly
one grammatical bestmove (`0000` allowed for terminal positions).

## Results

| Run | Cases | Accepted (no position error) | Timeouts | Non-zero exits | Unexpected stderr | Bad stdout |
|---|---|---|---|---|---|---|
| Release MSVC (`build/full-release-gpu`) | 2000 | 1009 | **0** | **0** | **0** | **0** |
| MSVC ASan (`build/asan-reliability`) | 500 | 237 | **0** | **0** | **0** | **0** |

Commands:

```powershell
$env:KOI_NNUE_PATH = "<promoted net>"
python tests/python/reliability/fen_fuzz.py `
  --engine build/full-release-gpu/koi-engine.exe --cases 2000 --workers 6 `
  --out artifacts/plans/reliability-2026-10-31/fen-fuzz.json
# ASan run: --engine build/asan-reliability/koi-engine.exe --cases 500 --workers 4
```

No engine change was required; the fuzzer is retained as a repeatable
adversarial-input tool. It complements (does not replace) the deterministic
fuzz corpus and the ASan/soak legs.
