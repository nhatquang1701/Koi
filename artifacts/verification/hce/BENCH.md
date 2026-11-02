# HCE bench evidence (built-in suite, depth 5)

Command: `koi-bench --hce --depth 5 --profile-json ...`, Threads 1, cold 512 MB hash.

| revision | accepted moves | nodes | qnodes |
| --- | --- | --- | --- |
| v1 | 62/64 | 4,283,112 | 286,564,941 |
| v2 | 60/64 | 4,103,416 | 262,506,776 |

The v1/v2 miss lists are the tactical fixtures that need iteration; the accepted-move
fixture is a regression guard, not a strength claim. Strength decisions use Mac
equal-time HCE-vs-HCE gates.
