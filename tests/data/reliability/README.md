# Reliability fuzz fixtures

These text fixtures feed the dependency-free randomized fuzzers
(en_fuzz.py, move_fuzz.py, go_fuzz.py). They were generated **once** with
python-chess from deterministic random legal playouts; they are not copied from
third-party games, opening books, or test suites, and python-chess is not
required at runtime.

- alid-fens.txt — 607 FENs: a small set of reference positions plus random
  legal playouts (0–120 plies) with 
andom.Random(20261101 + seed).
- alid-move-lists.txt — 400 UCI move sequences from random legal games
  (1–140 plies) with 
andom.Random(777000 + seed).

Generation sketch (run once, output committed):

`python
import chess, random
for seed in range(600):
    board, rng = chess.Board(), random.Random(20261101 + seed)
    for _ in range(rng.randint(0, 120)):
        legal = list(board.legal_moves)
        if not legal: break
        board.push(rng.choice(legal))
    print(board.fen())
`

Fuzzers mutate fixture positions/moves and generate malformed inputs from the
stdlib only; the fixtures exist so CI needs no third-party chess library.

SHA-256:
- valid-fens.txt: 450fd1fc2b655a705423db7df11c24dfb892c1a8a9f92bed5a9f691b1d362943
- valid-move-lists.txt: 096c55a7ca0d89cc57f111fb7a22b5c6543f576305d85025064fc26b829c98e0
