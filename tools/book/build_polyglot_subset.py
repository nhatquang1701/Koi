"""Build a compact Polyglot book from a reachable subset of a larger book.

Motivation: very large Polyglot books (for example Cerebellum Light, ~164 MB)
exceed the engine's fast-load book contract and would be re-read on every engine
process a GUI spawns.  This tool walks the source book breadth-first from the
start position up to a ply limit and keeps the highest-weight moves for each
reachable position, producing a small, standard Polyglot file that preserves the
source book's opening choices (highest weight == the book's preferred move).

Source entries are copied verbatim (key, raw move, weight, learn), so no move
re-encoding is involved.

Usage:
    python3 tools/book/build_polyglot_subset.py \
        --source Cerebellum_Light_Poly.bin --output koi-cerebellum-opening.bin \
        --max-ply 16 --max-positions 300000 --top-moves 3
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
from collections import deque

import chess
import chess.polyglot

HEADER = struct.Struct(">QHHI")


def build(source: pathlib.Path, output: pathlib.Path, max_ply: int,
          max_positions: int, top_moves: int, min_weight: int,
          max_size_bytes: int) -> None:
    reader = chess.polyglot.open_reader(str(source))
    try:
        start = chess.Board()
        queue: deque[tuple[chess.Board, int]] = deque([(start, 0)])
        seen: set[int] = {chess.polyglot.zobrist_hash(start)}
        records: list[tuple[int, int, int, int]] = []
        while queue and len(seen) <= max_positions:
            board, ply = queue.popleft()
            entries = []
            for entry in reader.find_all(board):
                if entry.weight < min_weight:
                    continue
                move = entry.move
                if move is None or not board.is_legal(move):
                    continue
                entries.append(entry)
            if not entries:
                continue
            entries.sort(key=lambda e: (-e.weight, e.move.uci()))
            for entry in entries[:top_moves]:
                records.append((entry.key, entry.raw_move, entry.weight, entry.learn))
            if ply + 1 >= max_ply:
                continue
            for entry in entries[:top_moves]:
                child = board.copy(stack=False)
                child.push(entry.move)
                key = chess.polyglot.zobrist_hash(child)
                if key in seen:
                    continue
                seen.add(key)
                queue.append((child, ply + 1))
        records.sort(key=lambda r: (r[0], -r[2], r[1]))
        # Keep the file inside the engine's fast-load contract by dropping the
        # lightest tail entries if the subset grew past the budget.
        while records and len(records) * HEADER.size > max_size_bytes:
            records.pop(0)
        with output.open("wb") as handle:
            for key, raw_move, weight, learn in records:
                handle.write(HEADER.pack(key, raw_move & 0xFFFF, weight & 0xFFFF,
                                         learn & 0xFFFFFFFF))
        print(f"positions={len(seen)} entries={len(records)} "
              f"bytes={output.stat().st_size} mul16={output.stat().st_size % 16 == 0}")
    finally:
        reader.close()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--max-ply", type=int, default=16)
    parser.add_argument("--max-positions", type=int, default=300_000)
    parser.add_argument("--top-moves", type=int, default=3)
    parser.add_argument("--min-weight", type=int, default=1)
    parser.add_argument("--max-size-bytes", type=int, default=16 * 1024 * 1024 - 16)
    args = parser.parse_args(argv)
    build(args.source, args.output, args.max_ply, args.max_positions,
          args.top_moves, args.min_weight, args.max_size_bytes)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
