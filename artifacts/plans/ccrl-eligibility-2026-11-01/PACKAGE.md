# CCRL testing package — Koi 1.1.5-dev (2026-11-01)

Generated Windows x86-64 package for CCRL testers. It contains only the engine
binaries, the gated NNUE net, licenses and the provenance/settings README — **no
book, no test suite, no developer tools, no Stockfish**.

| Item | Value |
|---|---|
| Directory | `artifacts/packages/ccrl-20261101/` |
| Archive | `koi-engine-1.1.5-dev-2708608-win-x86_64-ccrl.zip` |
| Archive SHA-256 | `2f3b8c8f1fbe0e287e5daa755401b59abdd0e4d6aaaf70ee2287d2bd6f9edd56` |
| Engine source HEAD | `2708608` (engine code `9336a38`) |
| Handshake | `id name Koi 1.1.5`, `info string build 2708608a7ff4` |
| Net | `nnue/koi.nnue`, SHA-256 `3e75e6d8…829da` (see `nnue/koi.nnue.sha256`) |
| Contents | `koi-engine.exe`, `koi-engine-avx2.exe`, `koi-engine-avx512.exe`, `nnue/*`, `README-CCRL.txt`, `LICENSE`, `licenses/*`, `SHA256SUMS` |
| Smoke test | extracted archive boots, auto-discovers `nnue/koi.nnue`, answers the handshake, sets Hash 256 / Threads 1 / OwnBook false / Ponder false, returns one legal bestmove, exit 0 |

The zip and checksums live under git-ignored `artifacts/` (generated binaries are
not committed). Regenerate with:

```powershell
# rebuild the three engine variants from a clean current tree with
# C:\Users\ntATh\koi-run\koi-msvc.cmd, then copy into the package directory and
# refresh SHA256SUMS + Compress-Archive + sha256 sidecar.
```

CCRL message template and the recommended settings are in `SUBMISSION.md`;
1000-game evidence is in `EVIDENCE.md`; provenance in `PROVENANCE.md`.
