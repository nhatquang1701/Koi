# Generated artifacts

This directory is the repository-local home for local verification reports,
match logs, training runs, packages, and manifests.

## What is tracked

`/artifacts/*` is ignored in `.gitignore` so benchmark dumps, training corpora,
match PGNs/JSONs, packages, and raw logs remain local and out of Git history.
Only this guide and tiny shared fixture manifests (such as
`artifacts/manifests/elo-anchors.json`) live here in version control.

## Common local subdirectories

| Directory | Contents |
| --- | --- |
| `manifests/` | Elo anchor config and local organization manifests |
| `matches/` | local UCI and engine-match reports/PGNs |
| `networks/` | local promoted `.nnue` networks and metadata sidecars |
| `packages/` | locally built release archives |
| `stability/` | local stability match runs and soak logs |
| `training/` | local corpora, trained networks, and run logs |
| `verification/` | local release, performance, and compatibility reports |

Tools create these directories automatically when run locally. Do not commit
generated outputs under `artifacts/`.
