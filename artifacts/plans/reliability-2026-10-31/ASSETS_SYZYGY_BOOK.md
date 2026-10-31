# Syzygy auto-discovery + bot book/tablebase installation (2026-10-31)

## Engine: automatic Syzygy directory discovery (commits `ebc4662`)

When `SyzygyPath` has never been set, `UciController` looks beside the engine
executable for a directory named `syzygy`, `syzygy345`, `tablebases`, or
`3-4-5` (in that order) that contains at least one `.rtbw`/`.rtbz` file
(`koi::discover_syzygy_directory`).  Discovery is cheap and silent; the tables
are loaded lazily at the first `isready`/`go` -- never while a search is running,
because Fathom is process-global -- and reported as
`info string syzygy detected: <path> (SyzygyPath not set, using it)` followed by
the existing `info string Syzygy: N-man tables at <path>` line.  Setting
`SyzygyPath` explicitly (including empty) disables discovery for the session; a
same-value explicit set loads the tables without the detection diagnostic.

Tables are never bundled or packaged; this is user-installed data.

### Verification

- Unit `syzygy_tablebase_tests`: `discovery finds a tables directory beside the
  executable` PASS on macOS (Windows local run is blocked by the sandbox
  temp-directory quirk; CI covers it).
- Real 5-man install, engine copied beside a symlinked `syzygy/` directory:
  - auto: `info string syzygy detected: /Users/quangnn/koi-bot/engines/syzygy
    (SyzygyPath not set, using it)` then `info string Syzygy: 5-man tables at ...`;
  - KQvK probe: `score cp 90000 ... tbhits 1`, legal `bestmove`;
  - explicit `setoption name SyzygyPath value <same path>`: only the
    `Syzygy: 5-man tables` line (no auto-detection diagnostic).
- `koi_engine_process` and the deterministic protocol fuzz corpus stay green.

## Mac Lichess bot: 5-man Syzygy installed

- Tables: `~/koi-bot/engines/syzygy` (290 files, 983,957,920 bytes), downloaded
  from the Lichess tablebase mirror (`tablebase.lichess.ovh`, 3-4-5 WDL+DTZ).
- Bot config: `SyzygyPath` explicitly points at that directory (the engine also
  auto-detects it); engine binary redeployed with the discovery feature.
- License note: Syzygy tables are user-installed data, not redistributed with
  Koi.

## Mac Lichess bot: Cerebellum Light opening book

- Source: https://workupload.com/file/THvyHB2pJMJ via the Chess Engines Diary
  download page (Playwright), archive `Cerebellum_light_200324.7z`,
  SHA-256 `63173249efd0c9d56e59905a7f7015cf29316cd66158d3275988a921e55c3dee`.
- Contents: `Cerebellum_Light_Poly.bin` (164,297,088 bytes, 10,268,568 Polyglot
  entries) plus README/license.
- **Format finding:** the full Cerebellum book uses Brainfish's extended
  repetition-aware key format; a standard Polyglot reader can traverse only the
  plain-keyed top tree (886 positions through 32 plies in this file).  The
  compact subset built from it (14 KB) is what a standard Polyglot engine,
  including Koi, can actually use; deeper entries are not addressable without
  implementing Brainfish's extension.
- **License:** CC BY-NC-SA 4.0 (non-commercial).  Used bot-locally only; it must
  never be bundled into Koi release packages (the release book stays CC0).
- Bot config: `OwnBook true`, `BookFile
  /Users/quangnn/koi-bot/engines/koi-cerebellum-opening.bin`, `BookDepth 32`,
  `BookRandom false` (highest-weight move = Cerebellum's preferred line),
  `BookSafety true`.
- Verified in-engine: startpos -> `info string book move e2e4 depth 0`; after
  1.e4 e5 2.Nf3 -> `b8c6`.

## Still open in this workstream

- Built-in release book: replace the current CC0 book with a custom-built CC0
  book (planned; the current book stays valid until then).
- The bot restart that applies the new `SyzygyPath`/book config runs between
  games via the autostart heavy-work guard; no game is interrupted.
