# Koi 1.1.5 release and absolute-strength estimate (2026-10-31)

## Release
- Tag: `1.1.5` (signed annotated) at commit `1e09504`; branch `koi-engine`
  force-pushed with the re-signed history (569+ commits) to the canonical
  repository `nhatquang1701/Koi`.
- GitHub release: https://github.com/nhatquang1701/Koi/releases/tag/1.1.5
  ("Koi Engine v1.1.5") with all three platform archives and `.sha256`
  sidecars:
  - `koi-engine-v1.1.5.zip` (Windows x64, SHA-256 `ba241fd0...`)
  - `koi-engine-v1.1.5-linux-x86_64.tar.gz` (Ubuntu 22.04 build, `6dcfbdf3...`)
  - `koi-engine-v1.1.5-macos-arm64.tar.gz` (Apple Silicon, `9d848beb...`)
  Every archive carries `nnue/koi.nnue` (promoted h128, gate decision
  `accept`), `book/koi.bin`, and a `koi-engine-package-v1` manifest with
  version `1.1.5`, per-file SHA-256 hashes, and clean-source provenance at
  commit `1e09504`.
- CI at the tagged commit: all four workflows green (Windows CTest, Linux CTest,
  macOS CTest, docs). The remote tag history was force-updated to the re-signed
  tags as planned at the version bump.
- Problem found and fixed during release verification: the shipped bot binary
  was five days old and predated every accepted upgrade; it was redeployed and
  the fixture-net/exe-smoke and TSan budgets were corrected so CI is green.

## Absolute strength (Mac arm64, 200 ms, Threads 4, Hash 512, promoted net)
- **1729 Elo, 95 % CI 1659-1795**, reliability moderate; SF19 UCI_Elo anchors
  1600/1800, 128 games (64 W / 10 D / 54 L), no book.
- The same engine source measured 1665 (95 % CI 1605-1726) in an earlier
  run, so the honest read is **~1700 +/- 35**; the release is not a strength
  change over the previously measured code.
- Deployment delta vs the old shipped binary (200 ms, 300 duplicate-free Mac
  games): 53.5 %, +24.6 Elo (LLR 0.83); on x86-64 the same redeploy also
  carries the +281.7 Elo CPUID fix, and it removes the old binary's 21 %
  live time-forfeit rate (0/24 in the 1+0 verification probe).
