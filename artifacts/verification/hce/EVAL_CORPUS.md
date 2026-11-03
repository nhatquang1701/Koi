# HCE vs promoted-NNUE static evaluation corpus

Corpus: 91 unique FENs (64 built-in bench positions + endgame/evaluation
fixtures). Both dumps use `koi-eval-features` on the same FENs from the
side-to-move perspective. NNUE column: promoted h128 network
`koi-v7-h128-official-20261025`; HCE column: clean HCE default.

| Metric | Value |
| --- | --- |
| Pearson correlation | 0.966 |
| HCE/NNUE regression slope | 1.208 |
| regression intercept | -45.5 cp |
| mean absolute difference | 140.1 cp |
| sign agreement | 93.4% |

This corpus is tactical/endgame-heavy and is a diagnostic sample, not a strength
claim. A low correlation indicates term/weight differences worth investigating
for calibration; it does not by itself justify changing search scale.
