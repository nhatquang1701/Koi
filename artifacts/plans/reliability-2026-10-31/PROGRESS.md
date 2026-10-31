# Reliability program progress

Updated: 2026-10-31 (round 3: sanitizer baseline, fail-closed assets, packaging policy)

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
| R5 sanitizer baseline | **verified** | Windows ASan: zero sanitizer reports (4 suites blocked only by local `%TEMP%`); Linux TSan CI job green at `b7ea994` |
| R6 fail-closed asset matrix | fixed | fuzz corpus asset matrix + Syzygy magic validation fail-closed (`c20c3a7`) |
| R7 Syzygy auto-discovery | fixed | `discover_syzygy_directory` + lazy load; real 5-man verification |
| R8 bundled release book | fixed | custom CC0 book, 752,320 B, sha `2ee7643d...`; packaging input updated |
| R9 release archives | fixed | developer tools removed from archives; engine + assets only; packaging tests green |
| R18 book hot-swap | fixed | valid -> truncated -> restored -> deleted auto-book in one session |
| R17 minimal-handshake/input abuse | fixed | empty input, pre-handshake commands and 20k-token searchmoves asserted |
| R16 EOF/stop-storm/ponder-reset | fixed | fuzz asserts partial-line EOF execution, exactly one bestmove from an active stop storm, and ucinewgame-during-ponder cancellation |
| R15 searchmoves contracts | fixed | process-level legal-subset / impossible-filter / duplicate-filter assertions |
| R13 terminal/forced-draw roots | fixed | process-level `bestmove 0000` and fivefold/analysis assertions in the fuzz corpus |
| R14 long soak | verified | 3000-round soak: 3001 bestmoves, bounded RSS, 39.2 s |
| R12 option churn | fixed | `koi_protocol_option_churn_process`: five option-change-during-search scenarios (Hash, Threads/MultiPV, EvalFile, BookFile/SyzygyPath, Hash burst) |
| R10 resource-churn soak | fixed | `koi_protocol_soak_process`: 60 rounds of Threads/Hash churn, searches, RSS bound, one legal bestmove per search, clean shutdown; heavy + reliability labels |

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

## Round 3 actions (2026-10-31)

- Syzygy fail-closed hardening: table magic validation on auto-discovery and
  explicit single directories; corrupt installs disabled with one diagnostic
  (`c20c3a7`); real 5-man tables still load; corrupt dir rejected silently.
- ASan baseline recorded (`build/asan-reliability`): zero sanitizer reports.
- Release archives now ship engine binaries only (`270b77a`).
- GitHub Pages fixed after the repository rename: `site_url`/README links moved
  to https://nhatquang1701.github.io/Koi/ and redeployed from a docs-only
  branch; sitemap now has 22 `/Koi/` URLs, release notes page returns 200.

## Pushed and verified (2026-10-31 afternoon)

- Branch `koi-engine` pushed from `bd8f3ec` to `b7ea994` (15 local commits: reliability
  fixes, fuzz+soak corpus, Syzygy auto-discovery + magic validation, custom CC0
  book, engine-only release archives, Pages URL fix, documentation audit).
- Full CI at `b7ea994` **all green**: Windows CTest (41177919078), Linux CTest
  (41177919034, including the `linux-tsan` job), macOS CTest (41177918926),
  Deploy documentation (41177918920), Dependency Graph.
- GitHub Pages now serves the corrected documentation at
  https://nhatquang1701.github.io/Koi/ (ENGINE_SPEC page shows the 2026-10-29
  full-budget movetime rule and 76-test count; release notes page 200).
