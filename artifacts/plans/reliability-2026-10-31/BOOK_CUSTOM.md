# Custom bundled book (2026-10-31)

## Choice

Koi's bundled release book is now a custom-built CC0 book derived from the
Lichess standard-game database dumps (CC0 1.0), replacing the earlier
chess-openings-derived book.  This gives the book real strong-human-game weights
and deeper lines while staying redistributable in releases.

## Build

- Tool: `tools/book/build_database_book.py` (new, committed).
- Sources: `lichess_db_standard_rated_2013-01..2014-01.pgn.zst`
  (https://database.lichess.org/standard/, CC0), 620 MB compressed.
- Filter: both players rated >= 1800, average >= 2000, base time control
  >= 180 s, standard chess only.
- Indexed: first 20 plies, weight = number of qualifying games, entries with
  at least 2 games.
- Scan: 4,084,023 games, 35,606 qualifying games; 284,030 positions,
  47,020 entries.
- Output: 752,320 bytes, SHA-256
  `2ee7643d193a285978c0c56b97f7d9d280aef9fd2d12c984a472b56df2d0a923`.
- Installed as `artifacts/book/book.bin` on Windows and the Mac (the packaging
  input); the previous book is kept as `artifacts/book/book-chessopenings-legacy.bin`.

## Verification

Engine copied beside the book and `koi.nnue` (book auto-detected):
- startpos -> `info string book move e2e4 depth 0`;
- after 1.e4 e5 2.Nf3 -> `b8c6`;
- after 1.d4 d5 2.c4 -> `e7e6`.
All legal, protocol-clean, and the book is auto-used without `OwnBook` (as
documented in §8.1).

## Bot note

The Mac Lichess bot deliberately uses **Cerebellum Light** (bot-local,
CC BY-NC-SA, never packaged) rather than this CC0 book; the release/bundled book
and the bot's book are independent choices.

## License

Lichess database dumps are CC0 1.0; the generated Polyglot book carries a
`book/COPYING.txt` notice in release packages.  No non-commercial book is
included in Koi releases.
