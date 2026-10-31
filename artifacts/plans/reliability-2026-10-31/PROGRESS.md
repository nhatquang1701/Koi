# Reliability program progress

Updated: 2026-10-31 (after the asset workstream landed)

## Commits (signed, local; push deferred until the user says "version bump")

- `8384f3c` docs(reliability): plan, baseline fuzz, issue triage
- `2706418` fix(uci): bound protocol input lines at 1 MiB + unit regression
- `660f885` test(uci): deterministic protocol fuzz corpus + `reliability` label
- `584106a` test(uci): interruption/ponder/resize/EOF lifecycle storms
- `ebc4662` feat(syzygy): auto-detect table directories beside the engine
- `17740c7` feat(tools): compact Polyglot opening-subset builder
- `2d31133` docs(reliability): Syzygy discovery + bot book/tablebase setup report
- `34f43f6` feat(book): custom CC0 Lichess-database bundled book

## Issue status

| Issue | Status | Evidence |
| --- | --- | --- |
| R1 unbounded protocol line | fixed | bounded reader + `bounded protocol line` test; 2 MiB e2e |
| R2 deterministic contract harness | fixed | `koi_protocol_fuzz_process` (~8 s, `reliability` label, every CI leg) |
| R3 extreme option contract | verified-pinned | existing unit tests (hash cap/policy, threads validation, MultiPV) |
| R4 lifecycle exactly-once races | fixed | lifecycle storms in the fuzz corpus |
| R5 sanitizer baseline | **open** | ASan tree `build/asan-reliability` configured/built; runs pending |
| R6 fail-closed asset matrix | **open** | next: malformed/truncated/replaced net/book/syzygy matrix |
| R7 Syzygy auto-discovery | fixed | `discover_syzygy_directory` + lazy load; real 5-man verification |
| R8 bundled release book | fixed | custom CC0 book, 752,320 B, sha `2ee7643d...`; packaging input updated |

## Asset workstream (user requests, done this session)

- Mac bot: 5-man Syzygy installed (`~/koi-bot/engines/syzygy`, 290 files,
  983,957,920 B), `SyzygyPath` set, engine redeployed with auto-discovery,
  verified `5-man tables ... tbhits 1`.
- Mac bot book: Cerebellum Light (CC BY-NC-SA 4.0, bot-local only) downloaded and
  verified (archive SHA-256 `63173249...`); the standard-Polyglot-accessible top
  tree was converted to a 14,176 B `koi-cerebellum-opening.bin`; bot config
  `OwnBook=true BookFile=... BookDepth=32 BookRandom=false BookSafety=true`;
  engine verified `book move e2e4 depth 0`, `b8c6` after 1.e4 e5 2.Nf3.
- Bundled release book: custom CC0 book from Lichess DB dumps; installed as
  `artifacts/book/book.bin` on Windows and Mac; docs + `BOOK_CUSTOM.md`.

## Next

1. Run the ASan unit label (`build/asan-reliability`, `-L unit -LE heavy`) and
   record/triage; then TSan on Linux where available (R5).
2. Fail-closed asset matrix: malformed/truncated/replaced NNUE, book, Syzygy,
   including auto-discovery corner cases (R6).
3. Soak: repeated Hash/Threads changes, book/syzygy reload churn, RSS bound,
   orphan-free shutdown.
