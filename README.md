# Koi NNUE Network: stage2-dual-f01-s95

Published via signed Git tag `nnue/stage2-dual-f01-s95` (standalone network artifact; not a GitHub Release).

## Download

- Raw file URL: `https://raw.githubusercontent.com/nhatquang1701/Koi/refs/tags/nnue/stage2-dual-f01-s95/stage2-dual-f01-s95.nnue`
- Size: 2360971 bytes
- SHA-256: `d7d73645672ca23c4252d8548be65694b4d6d48f6683ca1b00d9c16972a47e41`
- Payload SHA-256: `3d5b2910e97bc4ff46411560af49434bd5e1232a98e99ce90f22f1b23f91a940`

## Architecture

- Container format: `v4-dual` (`version: 4`, `perspectives: 2`, `activation: crelu-pair-dual`)
- Feature set: `halfka-king-bucket-v1`
- Topology: `9216 -> 128 x 2 -> 8` output buckets
- Quantization: `int16/int8` (`hidden_shift: 7`, `output_shift: 12`, `output_scale: 0.95`)
- Lineage: output head rescaled (`0.95`) from `stage2-dual-f01` (fine-tuned from `koi-v7-h128-d8c-20261027` on `combined-v2-dual.bin`, 3,387,289 rows)

## Match Verification

Compared against `koi-v7-h128-official-20261025.nnue` on macOS arm64 (`Koi 1.1.6`, `Threads=1`, `Hash=256`, `OwnBook=false`):

- **200 ms screen (80 games, paired):** 36 W / 17 D / 27 L (55.6%)
- **1+0 SPRT [0, 10] (`alpha=beta=0.05`, 789 games, 0 aborted):** 263 W / 333 D / 193 L (**+30.91 Elo**, `LLR = +2.956512 >= +2.944439`, **accept**)

Verify checksum after download:

```sh
sha256sum -c stage2-dual-f01-s95.sha256
```
