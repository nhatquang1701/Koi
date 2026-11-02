# HCE bench evidence (built-in suite, depth 5)

Command: `koi-bench --hce --depth 5 --profile-json ...`, Threads 1, cold 512 MB hash.

| revision | accepted moves | nodes | qnodes |
| --- | --- | --- | --- |
| v1 | 62/64 | 4,283,112 | 286,564,941 |
| v2 | 60/64 | 4,103,416 | 262,506,776 |

The v1/v2 miss lists are the tactical fixtures that need iteration; the accepted-move
fixture is a regression guard, not a strength claim. Strength decisions use Mac
equal-time HCE-vs-HCE gates.

## Native-input HCE throughput (Windows, depth 5, 64 positions)

| Path | elapsed | NPS |
| --- | --- | --- |
| cached `PositionFeatures` (v1) | 363.1 s | 801,077 |
| native board state (v5) | 377.6 s | 749,877 |
| native + single pawn-attack pass | 371.8 s | 761,631 |

This is a measurement baseline, not a strength claim; the Mac equal-time screen for
the native refactor was neutral (+16.4 Elo, LLR 0.106).
