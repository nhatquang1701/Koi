# ASan protocol-corpus flake investigation — 2026-11-01

## Observation
While adding the malformed-Polyglot-move case (R77), one ASan run of
`koi_protocol_fuzz_process` failed after **124.7 s** with no retained
AddressSanitizer output (`--output-on-failure` summary only; the log had already
been overwritten by the next successful run). The test passed on the immediate
rerun (206.4 s).

## Follow-up
Five additional ASan runs were executed with per-run logs preserved:

| Run | Result | Duration |
|---|---|---|
| repeat 1 | pass | 200.63 s |
| repeat 2 | pass | 205.14 s |
| repeat 3 | pass | 220.11 s |
| two-run `--repeat until-fail:2` | pass | 204.96 s each |
| immediate rerun after the failure | pass | 206.40 s |

- **0 AddressSanitizer/SEGV/heap-use/SUMMARY reports** in any of the six passing
  runs.
- The single failure was **not reproducible** in 6 further attempts (~20 min of
  ASan execution).
- Logs: `asan-fuzz-repeat-{1,2,3}.log` (CTest output) and
  `asan-fuzz-repeat-{1,2,3}-lasttest.log` (LastTest.log copies).

## Assessment
No evidence of a code defect; the most likely cause is a transient
environment/runtime issue (ASan startup/AV/scanner interaction, file lock on a
freshly written 16 MiB book fixture, or a PowerShell child-process hiccup). The
CI Windows sanitizer job now runs the same protocol corpus on every push (R76),
which will detect a recurrence with a retained report.

Do not weaken the corpus or add blanket retries based on this observation.
