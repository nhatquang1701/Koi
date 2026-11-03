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

## v6 (rook behind passed pawn) vs v3

Mac M4 arm64, 100 ms equal-time, 64 games, 32 openings.

| Side | W/D/L | Score | Elo | LLR |
| --- | --- | --- | --- | --- |
| White | 13/11/8 | 57.81% | +54.7 | 0.360 |
| Black | 8/13/11 | 45.31% | -32.7 | -0.373 |
| Combined | 21/24/19 | **51.56%** | **+11.0** | **-0.014** |

Decision: neutral; reverted to keep the v3 baseline lean.

## v7 (explicit tapered king PST) vs v3

Mac M4 arm64, 100 ms equal-time, 64 games, 32 openings.

| Side | W/D/L | Score | Elo | LLR |
| --- | --- | --- | --- | --- |
| White | 12/12/8 | 56.25% | +43.7 | 0.282 |
| Black | 9/12/11 | 46.88% | -21.7 | -0.262 |
| Combined | 21/24/19 | **51.56%** | **+11.0** | **0.020** |

Decision: neutral within noise. The explicit king tables are retained as tuning
infrastructure (mg/eg regimes are now independently editable), not as a strength
claim.

## Term-scale tuning screens (v8 tuning seam, 100 ms, 64 games)

All screens use the same binary with `KOI_HCE_*_SCALE` wrappers against the
default-100 baseline; only the named term group changes.

| Variant | W/D/L | Score | Elo | LLR | Decision |
| --- | --- | --- | --- | --- | --- |
| passed pawns 125% | 20/22/22 | 48.44% | -10.9 | -0.336 | neutral/negative, default kept |
| mobility 125% | 20/16/28 | 43.75% | -43.7 | -0.758 | negative, default kept |

The internal tuning seam (`KOI_HCE_PASSED_SCALE`, `..._MOBILITY_SCALE`,
`..._KING_SAFETY_SCALE`, `..._PAWN_SCALE`, `..._TEMPO_SCALE`) is retained as
development infrastructure; production defaults are 100 and no strength change
is claimed.

| king safety 125% | 22/23/19 | 52.34% | +16.8 | 0.107 | neutral-positive, default kept |
| pawn structure 75% | 19/19/26 | 44.53% | -38.2 | -0.710 | negative, default kept |

| king safety 175% | 26/18/20 | 54.69% | +33.2 | 0.420 | screen positive but **126-game confirm was 46.43%, -24.9 Elo, LLR -1.058**; default kept |

Protocol note: the 64-game screens are noisy and must not change defaults. Any
screen-positive variant needs a >=126-game confirm, and only a confirmed positive
is promoted; king-safety 175% demonstrates the failure mode.

## v9 (explicit tables for all piece PSTs) vs v8

Mac M4 arm64, 100 ms equal-time, HCE both sides, `Threads=1`, `Hash=64`.

| Gate | Openings | Games | W/D/L | Score | Elo | LLR |
| --- | --- | --- | --- | --- | --- | --- |
| screen | 32 | 64 | 23/24/17 | 54.69% | +33.0 | 0.398 |
| confirm | 63 | 126 | 42/41/43 | 49.60% | -2.8 | -0.314 |

Decision: neutral in the confirmation. The explicit tables are retained as
tuning infrastructure (mg/eg arrays are now editable everywhere), not as a
strength gain.

## Evaluation-scale screens (v10 tuning seam)

All against the default 100% HCE output scale, 100 ms equal-time unless noted.

| Variant | Gate | Games | W/D/L | Score | Elo | LLR |
| --- | --- | --- | --- | --- | --- | --- |
| scale 125% | screen | 64 | 12/28/24 | 40.63% | -67.7 | -1.788 |
| scale 80% | screen | 64 | 20/29/15 | 53.91% | +27.2 | 0.329 |
| scale 80% | confirm | 126 | 44/45/37 | 52.78% | +19.3 | 0.313 |
| scale 80% | large | 300 | 88/114/98 | 48.33% | -11.6 | -1.723 |

Decision: no scale change. The short positive run did not survive the 300-game
confirmation; default 100 kept. The `KOI_HCE_EVAL_SCALE` knob remains internal
tuning infrastructure.

## v11 (defended pawn threats) vs v10

Mac M4 arm64, 100 ms equal-time, HCE both sides.

| Gate | Games | W/D/L | Score | Elo | LLR |
| --- | --- | --- | --- | --- | --- |
| 32-opening screen | 64 | 20/27/17 | 52.34% | +16.5 | 0.274 |
| 63-opening confirm | 126 | 41/40/45 | 48.41% | -11.2 | -0.657 |

Decision: negative in confirmation; reverted to the v10 default.

## v12 (endgame mop-up) vs v10

Mac M4 arm64, 100 ms equal-time, 32 openings, 64 games.

| Side | W/D/L | Score | Elo | LLR |
| --- | --- | --- | --- | --- |
| White | 7/13/12 | 42.19% | -54.7 | -0.579 |
| Black | 9/16/7 | 53.13% | +21.7 | 0.121 |
| Combined | 16/29/19 | **47.66%** | **-16.5** | **-0.458** |

Decision: negative screen; reverted without a confirmation run.

| threat weight 150% | 17/34/13 (64) | 53.13% | +21.7 | 0.270 | screen |
| threat weight 150% | 43/38/45 (126) | 49.21% | -5.5 | -0.482 | confirm negative; default kept |

| piece terms 150% | 15/32/17 (64) | 48.44% | -10.9 | -0.399 | screen negative; default kept |

## v14 (weighted king-attack model) vs v13

Mac M4 arm64, 100 ms equal-time, 32 openings, 64 games.

| Side | W/D/L | Score | Elo | LLR |
| --- | --- | --- | --- | --- |
| White | 12/7/13 | 48.44% | -10.9 | -0.137 |
| Black | 7/13/12 | 42.19% | -54.7 | -0.579 |
| Combined | 19/20/25 | **45.31%** | **-32.8** | **-0.716** |

Decision: negative screen; reverted without a confirmation run.

| scale 110% | screen | 64 | 14/30/20 | 45.31% | -33.5 | -0.745 |

All tested output scales (80/110/125/150) are non-positive; the default 100%
scale is final unless a different score-calibration mechanism is found.

## v15 (NNUE-fitted PST correction) vs v13

Static-corpus fit: PST-only ridge correction on 12k generated positions against the
promoted NNUE static eval. HCE->NNUE corpus MAE improved 163.3 -> 148.2 cp
(validation 145.8 cp), with material untouched and the unit suite green.

Mac game screen, 100 ms, 32 openings, 64 games:

| Side | W/D/L | Score | Elo | LLR |
| --- | --- | --- | --- | --- |
| White | 11/11/10 | 51.56% | +10.9 | 0.007 |
| Black | 7/9/16 | 35.94% | -100.4 | -0.864 |
| Combined | 18/20/26 | **43.75%** | **-44.8** | **-0.857** |

Decision: negative game screen; reverted despite better static-corpus agreement.
Static-fit agreement with NNUE is not a playing-strength proxy.

## Tempo-weight screen

| Variant | Gate | Games | W/D/L | Score | Elo | LLR |
| --- | --- | --- | --- | --- | --- | --- |
| tempo 200% | screen | 64 | 22/27/15 | 55.47% | +41.1 | 0.624 |
| tempo 200% | confirm | 126 | 37/51/38 | 49.60% | -2.8 | -0.433 |

Decision: neutral in confirmation; default tempo weight kept. This closes the
single-group tuning matrix (passed, mobility, king safety, pawns, tempo,
threats, piece terms, eval scale) — no group variant survived confirmation.
