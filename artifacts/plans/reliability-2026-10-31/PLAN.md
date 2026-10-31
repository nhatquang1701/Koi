# Koi reliability program (2026-10-31)

Goal: make the engine effectively crash-free under adversarial input, resource
stress and unusual GUI behaviour, and make the UCI controller contract-perfect.
Continuous program; never version-bump/tag/release without explicit approval.

## Pillars

1. **Crash/UB elimination** — every reachable input path returns a legal,
   documented behaviour: no crash, no hang, no unhandled exception, no
   out-of-bounds/UB, no unbounded allocation from user input.
2. **UCI contract perfection** — `UciController` is the only protocol writer;
   every `go` yields exactly one legal `bestmove`; `isready` answers during
   search; `stop`/`ponderhit`/`quit`/EOF are race-free; options validate strictly
   and fail closed; handshake and option table always conform.
3. **Resource discipline** — Hash/Threads extremes, thread churn, TT resize,
   asset replacement, repeated matches, orphan-free shutdown, bounded RSS.
4. **Fail-closed assets** — missing/corrupt/mismatched NNUE, book and Syzygy
   files produce one diagnostic and a usable or explicitly-exiting engine, never
   a crash or protocol noise.
5. **Evidence-backed hardening** — every fix has a regression test; fuzz corpus,
   soak reports and sanitizer runs are recorded; CI stays green.

## Workstreams

| # | Workstream | First concrete deliverables |
| --- | --- | --- |
| W1 | UCI fuzz/transcript corpus | deterministic generator (malformed/oversized/binary/truncated lines, invalid FEN/moves/options, lifecycle storms); fast ctest label `reliability`; seed corpus |
| W2 | Controller contract tests | randomized sequence tests for go/stop/quit/isready/ponderhit/EOF; exactly-once legal-bestmove assertions under interruption; transcript grammar checks |
| W3 | Crash/UB fixes | one signed commit + regression test per reproduced issue (parser bounds, option validation, lifecycle races, exception containment, asset loaders) |
| W4 | Resource/soak | Hash 0/huge, Threads 0/1/max, resize churn, repeated `setoption`, multi-hour soak with RSS plateau, orphan-free shutdown |
| W5 | Sanitizers | ASan (Windows Debug), TSan (Linux), UBSan where available; record clean runs and triage |
| W6 | Documentation/lock-in | reliability matrix + recovery playbook; update ENGINE_SPEC/tests docs; periodic audit cycle |

## Phases

- **P0 (now): baseline & inventory.** Enumerate protocol entry points, option
  table, search lifecycle and asset loaders; run a first adversarial fuzz pass;
  record initial issues and reproduce each one.
- **P1: fuzz + contract harness in CI.** Land the generator/harness and a fast
  reliability subset so regressions are caught automatically.
- **P2: fix wave.** Work ISSUES.md top-down, one fix per signed commit with a
  regression test; rerun affected labels after each fix.
- **P3: sanitizer + soak evidence.** Clean ASan/TSan/UBSan runs on the supported
  configurations; multi-hour soak with bounded RSS and no orphans.
- **P4: lock-in & review.** Full reliability matrix, docs, and a final audit
  sweep of the controller and loaders.

## Acceptance

- No known crash/UB/hang in the issue list; fuzz corpus exits 0 (or documented
  exit codes) with legal bestmove and no protocol noise.
- ASan/TSan/LSan (+UBSan where supported) clean on the CI matrix; full CI green.
- Soak runs: bounded memory, no orphan engines/threads, deterministic
  `Threads=1` behavior preserved.
- Every fixed issue has a regression test; the contract invariants are pinned by
  tests, not just prose.
- No version bump/tag/release until the user explicitly asks.

## Evidence

`artifacts/plans/reliability-2026-10-31/ISSUES.md` (triage), per-issue reports,
fuzz corpus + seed list, sanitizer/soak logs and summaries. Small text evidence
is committed with `git add -f`; large logs stay local.
