# Lichess bot engine redeployed (2026-10-30)

## Finding
The Lichess bot was running `~/koi-bot/engines/koi-engine` built **2026-10-25
22:47**, i.e. before every accepted upgrade of this cycle:
- the CPUID-cache fix (+281.7 Elo, ~5x NNUE NPS) — 2026-10-26,
- the fixed-time iteration guard (+56.8 Elo, 1+0 confirmed) — 2026-10-28,
- the full-movetime budget fix (+31 to +42 Elo) — 2026-10-29,
- the HCE removal / NNUE-mandatory runtime — 2026-10-28.

The bot's recent game corpus (210 PGNs) shows **45 Koi time forfeits vs 5
opponent forfeits (21 % of all games Koi-flagged)**, across bullet through
classical controls (1+0 up to 1200+0). The old slow binary (pre-CPUID ~100k
NPS) and pre-guard time handling explain both shallow depth (user observed a
depth-7-only move missing a depth-8 fork) and the flags.

## Action
- Built current HEAD `9d7ee6f` on the Mac (engine SHA-256
  `02b3dd39d49fc739a7377f0740a98e9bd2fd4f6485bfae9a51a4edcf37cf9faa`,
  `info string build 9d7ee6f6b596`, NNUE autodiscovery from the bot directory).
- Backed up the old binary as `koi-engine.pre-deploy-20261030`; atomically
  replaced `koi-engine` between games (no game interrupted).
- Updated the greeting commit to `9d7ee6f6b596` and restarted the bot while it
  was game-free (autostart/launchd restored, bot running).
- Net unchanged: promoted h128 `koi-v7-h128-official-20261025` beside the
  engine; VERSION kept at 1.1.4 so the hourly release-update job does not
  overwrite this local build.

## Expected effect
- ~5x more nodes at fixed time on x86 (the Mac bot runs the arm64 build, where
  the CPUID fix is not the main factor; the time-safety and iteration-guard
  fixes still apply) and correct fixed-time budget usage.
- Time forfeits should drop materially; next step is to monitor the new PGNs
  (termination mix, game length) over the next sessions.

## Verification
- New engine handshake from the bot directory: `id name Koi 1.1.4`,
  `option name EvalFile type string default <autodiscovery>`,
  `info string build 9d7ee6f6b596`.
- `pgn`/`log` monitoring of post-restart games is the pending evidence.
