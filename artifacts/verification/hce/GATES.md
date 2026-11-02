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

## v2 vs v1, 50 ms movetime (300-game gate)

`tests/data/openings/openings-release-strength-160.txt`, 150 openings, 300 games.

| Side | W/D/L | Score | Elo | LLR |
| --- | --- | --- | --- | --- |
| White | 37/63/50 | 45.67% | -30.2 | -1.727 |
| Black | 53/46/51 | 50.67% | +4.6 | -0.191 |
| Combined | 90/109/101 | **48.17%** | **-12.8** | **-1.918** |

Decision: inconclusive but negative in both a 300-game 50 ms gate and the
20k-node diagnostic. The v2 term expansion is therefore reverted to the v1
baseline (`dccf13d`); individual features will be reintroduced one at a time
behind focused screens.

## v3 (passed-pawn king proximity/blocking) vs v1

Mac M4 arm64, clean HCE both sides, `Threads=1`, `Hash=64`, no book.

| Gate | Openings | Games | Score | Elo | LLR | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| 100 ms screen | 32 | 64 | 60.16% | +72.5 | 1.036 | 2 duplicate games |
| 100 ms confirm | 63 | 126 | 54.37% | +30.6 | 0.586 | no duplicates |
| 50 ms large | 150 | 300 | 53.67% | +25.6 | 1.195 | no duplicates |

Decision: consistently positive but still inconclusive at the 95% boundary.
The focused v3 change is retained as the current HCE baseline pending a larger/
longer confirmation; no promotion or Elo claim is made.

## v4 (backward pawns) vs v3

Mac M4 arm64, 100 ms equal-time, HCE both sides, `Threads=1`, `Hash=64`, no book.

| Gate | Games | W/D/L | Score | Elo | LLR |
| --- | --- | --- | --- | --- | --- |
| 32-opening screen | 64 | 16/34/14 | 51.56% | +11.3 | -0.163 |
| 63-opening confirm | 126 | 44/40/42 | 50.79% | +5.5 | -0.165 |

Decision: neutral; the backward-pawn term is reverted and v3 remains the baseline.

## v5 (native-input HCE) vs v3 (behaviour screen)

Mac M4 arm64, 100 ms equal-time, 32 openings, 64 games, HCE both sides.

| Side | W/D/L | Score | Elo | LLR |
| --- | --- | --- | --- | --- |
| White | 13/10/9 | 56.25% | +43.7 | 0.255 |
| Black | 11/9/12 | 48.44% | -10.9 | -0.149 |
| Combined | 24/19/21 | **52.34%** | **+16.4** | **0.106** |

Decision: neutral within noise. The native-input refactor is kept as an
architectural change (no shared NNUE feature container), not as a strength gain.
Windows depth-5 bench throughput for the native path: 749,877 NPS vs 801,077 for
the cached-feature v1 path on the same machine (minor overhead, same term set).
