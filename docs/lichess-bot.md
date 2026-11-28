# Running Koi as a Lichess Bot

[lichess-bot](https://github.com/lichess-bot-devs/lichess-bot) is the official
Python bridge that connects any UCI engine to Lichess. Koi implements the full
UCI surface required for competitive online bot play: clock-driven `go`
commands, adaptive network-overhead learning, `stop`, in-place
`ponder`/`ponderhit` conversion, and `MultiPV`.

`tools/bot/koi-bot.sh` wraps the whole setup so a bot can be installed, started,
stopped, and updated with single commands. It is written for macOS and is
verified on Apple Silicon; the same layout works on Linux with a different
release asset.

## Prerequisites

- A Lichess account upgraded to a **bot account**, plus an OAuth token with the
  `bot:play` scope (account menu: *Play with the Lichess API*). A token that is
  only allowed to play bot games is fine — that is the normal bot scope.
- **Python 3.11 or newer** (lichess-bot refuses older interpreters). macOS ships
  3.9/3.10, so build or install a newer one first; the script records whichever
  interpreter it used in `~/koi-bot/python-bin`.
- `git`, `curl`, and a Koi release for the host platform.

!!! tip "Bot token scope"
    Only an OAuth token with `bot:play` is needed; keep `~/koi-bot/token` readable solely by your user (`chmod 600`).

## Install

```bash
# 1. put the control script and the token in place (the token never leaves the host)
install -d -m 700 ~/koi-bot
cp tools/bot/koi-bot.sh ~/koi-bot/ && chmod +x ~/koi-bot/koi-bot.sh
printf '%s' 'lip_...' > ~/koi-bot/token
chmod 600 ~/koi-bot/token

# 2. install (clones the bridge, makes the venv, installs the newest Koi release)
~/koi-bot/koi-bot.sh install

# 3. start / stop / inspect
~/koi-bot/koi-bot.sh start
~/koi-bot/koi-bot.sh status
~/koi-bot/koi-bot.sh stop
```

`install` downloads the latest release package (verifying the `.sha256` sidecar on older releases or GitHub's release-asset SHA-256 `digest` metadata on newer releases, plus `koi nnue` payload checksum verification), installs
`koi` plus the shipped `.nnue` under `~/koi-bot/engines`, and writes
`~/koi-bot/config.yml` (mode 600) pointing `EvalFile` at that net with rated
bot-vs-bot matchmaking enabled (`KOI_BOT_MATCHMAKING=true` by default,
`KOI_BOT_MATCHMAKING_TIMEOUT=5` minutes).

## Commands

| Command | Effect |
| --- | --- |
| `koi-bot.sh install [--token T]` | clone/refresh the bridge, build the venv, install the newest engine, write the config |
| `koi-bot.sh start` | run the bot (launchd when installed, otherwise a pidfile) |
| `koi-bot.sh stop` | stop the bot and its engine processes |
| `koi-bot.sh restart` | stop, then start |
| `koi-bot.sh autostart` | start when idle/lightly loaded; pause under heavy work once any active game finishes |
| `koi-bot.sh sprt-mode [on\|off\|status]` | toggle SPRT match lock (disables autostart and stops the bot after any active game) |
| `koi-bot.sh status` | engine version, net, service state, last log lines |
| `koi-bot.sh reconfigure` | rewrite `config.yml` from current environment settings |
| `koi-bot.sh logs [N]` | tail the bot log |
| `koi-bot.sh update [--force]` | install the newest release and restart only when the tag changed |
| `koi-bot.sh install-launchd` | start at login, restart on crash, check for a new release hourly, and enable idle-aware autostart |
| `koi-bot.sh uninstall-launchd` | remove those agents |

## Staying on the newest release

`update` asks GitHub for `releases/latest`, compares the tag with the installed
`~/koi-bot/engines/VERSION`, and does nothing when they match. When a newer
release exists it downloads the macOS arm64 package, verifies its SHA-256 hash
(using the `.sha256` sidecar on legacy releases or GitHub's asset `digest` field
on v1.2.0+), extracts the engine, verifies the bundled network with
`koi nnue`, rewrites the config, and restarts the bot.

`install-launchd` registers three agents:

- `org.koi.lichess-bot` — runs the bot at login and restarts it after a crash;
- `org.koi.lichess-bot-update` — runs `update` every hour;
- `org.koi.lichess-bot-autostart` — runs `autostart` every 120 seconds so the bot yields CPU during heavy local builds/benchmarks without interrupting live games.

So every minor update or patch published on the repository reaches the bot
within the hour, with no manual step. Run `koi-bot.sh uninstall-launchd` to go
back to manual control.

## Layout

```
~/koi-bot/
  koi-bot.sh          # this control script
  token               # OAuth token, mode 600
  config.yml          # generated, mode 600 - edits are overwritten
  python-bin          # interpreter used for the venv
  engines/koi  # release binary
  engines/koi.nnue    # release network
  engines/VERSION     # installed release tag
  lichess-bot/        # upstream clone + .venv
  logs/bot.log        # bot output
  logs/update.log     # updater output
  games/              # PGN records of the bot's games
```

## Engine options used

The generated config sends only options Koi defines:

```yaml
uci_options:
  Threads: 4
  Hash: 512
  UCI_ShowWDL: true
  Contempt: 50
  Dynamic Overhead: true
  Move Overhead: 100
  EvalFile: "/Users/<you>/koi-bot/engines/koi.nnue"
```

!!! note "Absolute `EvalFile` path"
    Keep `EvalFile` absolute: the engine resolves relative network paths against its own working directory and falls back to `koi.nnue` beside the binary on failure. Setting `move_overhead: 300` in lichess-bot alongside `Move Overhead: 100` + `Dynamic Overhead: true` leaves a resilient buffer for Lichess HTTP latency spikes while keeping fast time controls agile.

## Opening book

You can use either the bundled CC0 `book.bin` shipped in release archives or
build/supply your own Polyglot book (for example with
`tools/book/build_verified_book.py` or `tools/build/install_book.ps1`, or a
local book such as Cerebellum Light) placed next to the engine binary as
`book.bin`:

```bash
cp path/to/your-book.bin ~/koi-bot/engines/book.bin
```

A book found beside the executable is used **automatically** — the engine
reports `info string book detected: …` and plays the book move without any
option. Setting `OwnBook=false` from a GUI disables it again, and `BookFile`
points at a different book. `BookDepth` (default 16), `BookRandom`, and
`BookSafety` tune how it is used.

Because the book is used before the search starts, it also saves clock time in
the opening, which matters most in short games.

## Notes and troubleshooting

- **`Unexpected engine output: 'info string NNUE enabled from …'`** — harmless.
  Koi reports the loaded network on `setoption`; lichess-bot logs unknown
  `info string` lines as a warning and keeps playing.
- **The bot plays without the net** — check `EvalFile` in `config.yml` is an
  absolute path that exists, then `koi-bot.sh logs` for the engine's own
  `info string EvalFile rejected: …`.
- **`unrecognized arguments: -c`** — older/newer lichess-bot versions spell the
  config flag `--config`; the script uses the current spelling.
- Koi plays standard chess only; leave the variants list at `standard`.
- Optional assets stay optional: a book beside the executable and a conventional
  Syzygy directory (`syzygy/`, `syzygy345/`, `tablebases/`, `3-4-5/`) are
  auto-detected; `OwnBook`/`BookFile` and `SyzygyPath` only override that.
  Release archives ship only the executables, `koi.nnue`, `book.bin`, and `LICENSE`.
- For analysis-style use (no clock, infinite search, MultiPV), a GUI is a better
  fit than a bot; see the [user guide](USER_GUIDE.md).
- **Desktop status window.** While the bot is online, `koi-bot.sh start` opens a
  Terminal window titled **LICHESS BOT RUNNING** showing the installed engine
  version, config and log path. The window closes automatically (and on
  `koi-bot.sh stop`) when the bot goes offline. It is opened through
  `open -a Terminal`, so no Automation permission is needed to open it; the
  close-on-exit step uses AppleScript and is silently skipped if macOS denies
  Automation access (the window then shows LICHESS BOT OFFLINE instead).
- **All time controls.** The generated config accepts bullet, blitz, rapid and
  normal challenges with `move_overhead: 300` ms, `Contempt: 50` (play for a
  win; accept a draw only when forced), `Dynamic Overhead: true`, and
  `Move Overhead: 100` ms (the engine scales bullet overhead proportionally while
  never dropping below the configured floor or auto-learned HTTP round trip). The last options
  are active from the first engine release that advertises them; older engines
  ignore unknown options.
- The bridge is AGPLv3; it is cloned at setup time and never vendored here.
