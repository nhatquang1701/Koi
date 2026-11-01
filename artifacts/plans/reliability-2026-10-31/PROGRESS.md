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
| R34 ASan unit label | pass* | 20/24 suites, zero ASan reports; 4 blocked by local %TEMP% quirk |
| R75 go fuzzer dependency fix | fixed | fixture-driven; local reliability label 7/7 |
| R74 reliability suite in Linux/macOS CI | added | workflow step runs `-L reliability -LE heavy` |
| R73 fixture-driven CI fuzzers | fixed | no python-chess dependency; policy test green; 4/4 in 56.3 s |
| R72 fuzzers in CTest/CI | pass (superseded by R73) | four randomized fuzzers registered; local 4/4 in 53.1 s |
| R71 max-size book load | pass | 16 MiB all-same-key book; book move e2e4; release+ASan |
| R70 non-regular DebugFile | fixed | FIFO/pipe DebugFile ignored; UCI responsive |
| R69 non-regular EvalFile | fixed | FIFO/named pipe rejected by type before open |
| R68 local suites with TEMP override | pass* | release 63/66 (3 Python measurement env failures); ASan 24/24 |
| R67 CI fixture regression | fixed | 272-byte canonical fixtures; all 20 syzygy unit tests pass locally |
| R66 post-fix ASan fuzz revalidation | pass | all four fuzzers clean on current ASan engine |
| R65 truncated Syzygy hang | fixed | canonical-size gate; reproduced hang gone; same-size/6-man residual |
| R64 200k asset-churn soak | pass | 200,001 bestmoves, 29,922 scored, 2,034.5 s, bounded RSS |
| R63 illegal crafted book move | pass | illegal entry never returned; legal fallback |
| R62 linked book assets | pass | hardlink/symlink detection; broken symlink fallback |
| R61 very long asset paths | pass | 100 KB values inert; release+ASan |
| R60 controller hash alloc failure | pass | allocation_failed, readyok, one bestmove; release+ASan |
| R59 unreadable EvalFile | pass | rejected once, evaluator preserved, legal search |
| R58 unreadable Syzygy tables | fixed | distinct `unreadable` diagnostic; probing disabled |
| R57 unreadable-book detection | fixed | denied book not announced; readable replacement detected/used |
| R56 post-Syzygy full-suite revalidation | pass* | release 59/66, ASan 20/24, 0 sanitizer reports; failures env-only |
| R55 asset-churn soak | pass | 30k rounds with SyzygyPath/BookFile churn, bounded RSS |
| R54 go/state-machine fuzz | pass | 1000 release + 300 ASan cases, 0 failures |
| R53 setoption parser fuzz | pass | 1000 release + 300 ASan cases, 0 failures |
| R52 move-list parser fuzz | pass | 2000 release + 500 ASan cases, 0 failures |
| R51 FEN parser fuzz | pass | 2000 release + 500 ASan cases, 0 failures |
| R50 Syzygy docs + sanitizer legs | fixed | ENGINE_SPEC path-list text corrected; ASan churn/pipe clean |
| R49 Syzygy path-list fail-closed | fixed | corrupt list component disables probing; empty dir not corrupt |
| R48 debug/register/TB option battery | fixed | debug/register/unknown/TB values inert; small Hash sizes safe |
| R47 book eligibility matrix | fixed | MultiPV>1/infinite/ponder/searchmoves bypass; normal restores |
| R46 analysis-mode book bypass/restore | fixed | analysis bypasses book; leaving analysis restores detection/use |
| R45 ready floods + extreme limits | fixed | 101 readyok during search; weird winc/binc/movestogo; max valid Threads/Hash/MultiPV |
| R44 ponder lifecycle races | fixed | immediate ponder stop, duplicate stop/ponderhit, stop-before-go |
| R43 control bytes/whitespace/ponder-searchmoves | fixed | NUL/control option values, tab commands, ponder+searchmoves |
| R42 slow-consumer backpressure + debug-log failures | fixed | undrained stdout recovers; invalid DebugFile safe |
| R41 transient locked book | fixed | first search falls back, released book detected/used; no negative cache |
| R40 suite revalidation | pass* | release 4/4; ASan fuzz clean; 58/66 local non-heavy, 8 environment quirks |
| R39 100k-round soak | pass | 100,001 bestmoves, 14969 scored, 1329 s, bounded RSS |
| R38 exhaustive option-invalid matrix | pass | all advertised options reject malformed values; Release + ASan green |
| R37 hot-add assets mid-session | fixed | book hot-add detected/used; corrupt Syzygy hot-add silently rejected |
| R36 malformed option/position forms | fixed | directory/empty/missing-value option forms; Release + ASan green |
| R35 book-size/EvalFile boundaries | fixed | 0/1/15/17 B and >16 MiB books unused; invalid EvalFile rejected |
| R34 ASan process legs | pass | fuzz/option-churn/broken-pipe/soak/engine-process/perft green; 2 known ASan timing tests |
| R33 early-completion soak fix | fixed | 30k rounds pass (30,001 bestmoves, 397 s, bounded RSS) |
| R32 ASan Fathom path-list OOB | fixed | sanitized SyzygyPath lists; ASan fuzz green |
| R31 mid-search asset mutation + destructive option storm | fixed |
| R30 CRLF/binary/path edges | fixed | CRLF, binary prefix, NUL-in-command, quoted/Unicode asset paths |
| R29 10k-round soak | pass | 10001 bestmoves, 1497 scored, 119 s, bounded RSS |
| R28 stdio/state-machine stress | fixed | closed stderr, 500x handshake flood, seeded 250-command storm |
| R27 book-option/handshake/limit contracts | fixed | book options during search, repeated handshake, contradictory go limits |
| R26 currmovenumber | fixed | interim lines emit `currmove ... currmovenumber N`; grammar pairing + process assertion |
| R25 quit-during-ponder | fixed | clean shutdown, no late bestmove |
| R24 long legal history | fixed | 64k-ply reversible history parses/searches; 80k plies fail closed |
| R23 ponder/clock contracts | fixed | stop-during-ponder one bestmove; idle ponderhit ignored (spec corrected); extreme clock limits safe |
| R22 SIGPIPE/GUI disconnect | fixed | POSIX SIGPIPE ignored; broken-pipe test on Windows and macOS |
| R21 extreme FEN/go limits | fixed | huge/negative clock fields and absurd go limits rejected/ignored safely |
| R20 analysis MultiPV scores | fixed | scored multipv 1-3 lines at one depth asserted |
| R19 option validation/EvalFile swap | fixed | invalid book/spin/movement values ignored; mid-search valid EvalFile swap installs once and next search legal |
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

- CI at `6089144` is fully green (Windows/Linux incl. TSan/macOS/docs), covering R16-R22 and the SIGPIPE hardening.


- Branch `koi-engine` pushed from `bd8f3ec` to `b7ea994` (15 local commits: reliability
  fixes, fuzz+soak corpus, Syzygy auto-discovery + magic validation, custom CC0
  book, engine-only release archives, Pages URL fix, documentation audit).
- Full CI at `b7ea994` **all green**: Windows CTest (41177919078), Linux CTest
  (41177919034, including the `linux-tsan` job), macOS CTest (41177918926),
  Deploy documentation (41177918920), Dependency Graph.
- GitHub Pages now serves the corrected documentation at
  https://nhatquang1701.github.io/Koi/ (ENGINE_SPEC page shows the 2026-10-29
  full-budget movetime rule and 76-test count; release notes page 200).
