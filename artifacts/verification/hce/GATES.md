# HCE gate evidence

Machine: Mac mini M4 arm64 (quiet), AppleClang 21, Release, `KOI_BUILD_MODULES=OFF`.
Binaries: `koi-v1` = `dccf13d` (HCE v1), `koi-v2` = `53d60e0` (HCE v2 terms).
Both sides run the clean HCE (`KOI_EVALUATOR=hce`), `Threads=1`, `Hash=64`,
no book, no Syzygy.

## v2 vs v1, 100 ms movetime (short gate)

`tests/data/openings/openings-curated-32.txt`, 32 openings, 64 games.

| Side | W/D/L | Score | Elo | LLR |
| --- | --- | --- | --- | --- |
| White | 10/11/11 | 48.44% | -10.9 | -0.163 |
| Black | 16/8/8 | 62.50% | +88.7 | +0.574 |
| Combined | 26/19/19 | **55.47%** | **+38.9** | **0.41** |

Decision: inconclusive; one duplicate game (63 effective of 64).

## v2 vs v1, 20,000 nodes (secondary diagnostic)

`tests/data/openings/openings-release-strength-160.txt`, 150 openings, 300 games.

| Side | W/D/L | Score | Elo | LLR |
| --- | --- | --- | --- | --- |
| White | 38/64/48 | 46.67% | -23.2 | -1.437 |
| Black | 50/53/47 | 51.00% | +6.9 | -0.116 |
| Combined | 88/117/95 | **48.83%** | **-8.1** | **-1.553** |

Decision: inconclusive. Node-limited results are not equal-time strength evidence;
the 100 ms short gate is positive but too small to decide. v2 remains the current
development baseline pending a larger equal-time gate (preferred at 20-50 ms for
HCE throughput) and further term/tuning iterations.

No promotion/rejection is claimed from either gate.
