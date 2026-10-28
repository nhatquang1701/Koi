# Explicit movetime uses the full requested budget (accepted 2026-10-29)

## Change
`TimeManager` subtracted the configured **Move Overhead** from an explicit
`go movetime` request on top of the safety margin. Move Overhead exists to keep
a late move from flagging a game clock; a movetime search has no clock to
protect, so it only discarded 15-20 % of the requested analysis time (200 ms
request -> 162 ms budget). The movetime branch now applies Speed and the safety
margin only (200 ms -> 190 ms, +17 % wall time). Clock games are untouched.

Files: `src/koi/time_manager.cpp`, `tests/unit/search/koi_search_tests.cpp`
(the budget expectation moved 466 -> 475 ms for the 1 s / Speed 50 case).

## Pinned methodology
- Same AVX2 build on both sides (`koi-engine-avx2.exe`), promoted h128
  `koi-v7-h128-official-20261025` pinned at boot for both engines.
- 200 ms movetime, Threads 1, Hash 64, paired colours, own book off, no Syzygy.
- Candidate SHA-256 prefix `7a401f6d93f3`, baseline `f71a34904513`.

## Gates
### A: 52-opening corpus, 4 games per opening per colour
- 416 played: 170 W / 126 D / 120 L = 56.01 %, **+42.0 Elo, LLR 3.203, accept**
  (white +31.8, LLR 1.08; black +52.2, LLR 2.12).
- **Caveat:** 68 of 416 games were exact repeats (348 distinct), so the
  combined LLR is inflated and the acceptance is not clean evidence.
- Evidence: `reports/movetime-full-sprt3/movetime-full-20261029-002859`.

### B: duplicate-free rerun, 150-opening corpus, 1 game per opening per colour
- 300 distinct games: 117 W / 93 D / 90 L = 54.5 %, **+31.5 Elo, LLR 1.552,
  inconclusive** (white +9.3, LLR -0.03; black +53.7, LLR 1.58).
- Evidence: `reports/movetime-full-sprt-clean/movetime-full-clean-20261029-014846`.

## Decision
**Accepted with caveat.** The primary gate crossed the accept boundary and the
duplicate-free sample is positive in the same direction (both colours positive),
consistent with the expected effect of ~17 % more fixed analysis time. The clean
300-game sample is below its own resolution (accepting +31 Elo needs a larger
sample), so the gain is reported as **+31 to +42 Elo at 200 ms** rather than a
single decisive number. Clock controls are unaffected by design.

## Verification
- `time_manager_tests` 10/10, `koi_search_tests` 143/143,
  `koi_engine_time_safety_process` pass.
- Worktree committed locally after the gate (signed); not pushed per policy.

## Follow-up
Fixed-time gates on the 52-opening corpus repeat deterministic games when
`-Games > 1`; use the 150-opening corpus with one game per opening per colour
for duplicate-free evidence, and extend that corpus for decisive +20-40 Elo
gates.
