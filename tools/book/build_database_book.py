"""Build a Polyglot opening book from Lichess standard-game database dumps.

Source: https://database.lichess.org (Creative Commons CC0 1.0).  The monthly
``lichess_db_standard_rated_YYYY-MM.pgn.zst`` dumps are streamed; only games whose
both players are rated above the thresholds and whose base time control is at
least classical/rapid length contribute.  Every qualifying game adds one count
to each (position, move) pair for the first ``--max-ply`` plies, so the book
weight is the number of strong games that played the move.

The output is a standard Polyglot ``.bin`` (16-byte big-endian entries sorted by
key), readable by any Polyglot engine.

Usage:
    python3 tools/book/build_database_book.py \
        --source '~/koi-run/lichess-db/*.pgn.zst' \
        --output artifacts/book/book.bin --min-elo 2000 --min-opponent 1800
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import io
import pathlib
import re
import struct
import sys

import chess
import chess.polyglot
import zstandard

HEADER = re.compile(r'^\[(\w+) "(.*)"\]\r?$')
COMMENT = re.compile(r"\{[^}]*\}")
VARIATION = re.compile(r"\([^()]*\)")
MOVE_NUMBER = re.compile(r"\d+\.(?:\\.\\.)?")
RESULTS = {"1-0", "0-1", "1/2-1/2", "*"}
PROMOTION_CODES = {chess.KNIGHT: 1, chess.BISHOP: 2, chess.ROOK: 3, chess.QUEEN: 4}
MAX_WEIGHT = 0xFFFF


def polyglot_castling_destination(move: chess.Move) -> int | None:
    if move.from_square not in (chess.E1, chess.E8):
        return None
    delta = move.to_square - move.from_square
    if delta == 2:
        return move.from_square + 3
    if delta == -2:
        return move.from_square - 4
    return None


def base_seconds(time_control: str) -> int:
    if not time_control or time_control == "-":
        return 0
    base = time_control.split("+")[0]
    if "/" in base:  # "40/300" style
        base = base.split("/")[-1]
    try:
        return int(base)
    except ValueError:
        return 0


def qualify(headers: dict[str, str], min_elo: float, min_opponent: int,
            min_base_seconds: int) -> bool:
    try:
        white = int(headers.get("WhiteElo", "0") or 0)
        black = int(headers.get("BlackElo", "0") or 0)
    except ValueError:
        return False
    if white < min_opponent or black < min_opponent:
        return False
    if (white + black) / 2 < min_elo:
        return False
    if base_seconds(headers.get("TimeControl", "")) < min_base_seconds:
        return False
    if headers.get("Variant", "Standard") not in ("Standard", "standard"):
        return False
    return True


def add_game(movetext: list[str], max_ply: int,
             book: dict[int, dict[int, int]]) -> None:
    text = " ".join(movetext)
    text = COMMENT.sub(" ", text)
    while True:
        cleaned = VARIATION.sub(" ", text)
        if cleaned == text:
            break
        text = cleaned
    text = MOVE_NUMBER.sub(" ", text)
    board = chess.Board()
    ply = 0
    for token in text.split():
        token = token.strip()
        if not token or token in RESULTS:
            continue
        try:
            move = board.parse_san(token)
        except ValueError:
            return
        if ply >= max_ply:
            return
        key = chess.polyglot.zobrist_hash(board)
        encoded_move = move
        if board.is_castling(move):
            rook_square = polyglot_castling_destination(move)
            if rook_square is not None:
                encoded_move = chess.Move(move.from_square, rook_square)
        encoded = ((encoded_move.to_square & 0x3F)
                   | ((encoded_move.from_square & 0x3F) << 6)
                   | (PROMOTION_CODES.get(encoded_move.promotion, 0) << 12))
        moves = book.setdefault(key, {})
        moves[encoded] = moves.get(encoded, 0) + 1
        board.push(move)
        ply += 1


def build(sources: list[str], output: pathlib.Path, min_elo: float,
          min_opponent: int, min_base_seconds: int, max_ply: int,
          min_count: int) -> None:
    book: dict[int, dict[int, int]] = {}
    games = qualified = 0
    for pattern in sources:
        for path in sorted(glob.glob(str(pathlib.Path(pattern).expanduser()))):
            stream = io.TextIOWrapper(
                zstandard.ZstdDecompressor().stream_reader(open(path, "rb")),
                encoding="utf-8", errors="replace")
            headers: dict[str, str] = {}
            movetext: list[str] = []
            state = "headers"
            for raw in stream:
                line = raw.rstrip("\r\n")
                if state == "headers":
                    if line.startswith("["):
                        match = HEADER.match(line)
                        if match:
                            headers[match.group(1)] = match.group(2)
                        continue
                    if line == "":
                        games += 1
                        if qualify(headers, min_elo, min_opponent, min_base_seconds):
                            state = "body"
                            movetext = []
                            qualified += 1
                        else:
                            state = "skip"
                        continue
                    continue
                if state == "body":
                    if line == "":
                        add_game(movetext, max_ply, book)
                        headers, movetext, state = {}, [], "headers"
                    else:
                        movetext.append(line)
                    continue
                if line == "":  # skip body
                    headers, state = {}, "headers"
            if state == "body":
                add_game(movetext, max_ply, book)
            print(f"processed {path}: games={games} qualified={qualified}", flush=True)
    entries: list[tuple[int, int, int]] = []
    for key, moves in book.items():
        for move, count in moves.items():
            if count >= min_count:
                entries.append((key, move, min(count, MAX_WEIGHT)))
    entries.sort(key=lambda item: (item[0], -item[2], item[1]))
    payload = bytearray()
    for key, move, weight in entries:
        payload += struct.pack(">QHHI", key, move, weight, 0)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(bytes(payload))
    print(f"games={games} qualified={qualified} positions={len(book)} "
          f"entries={len(entries)} bytes={len(payload)} "
          f"sha256={hashlib.sha256(payload).hexdigest()}")
    print(f"wrote {output}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", action="append", required=True,
                        help="glob of monthly .pgn.zst dumps (repeatable)")
    parser.add_argument("--output", required=True)
    parser.add_argument("--min-elo", type=float, default=2000.0)
    parser.add_argument("--min-opponent", type=int, default=1800)
    parser.add_argument("--min-base-seconds", type=int, default=180)
    parser.add_argument("--max-ply", type=int, default=20)
    parser.add_argument("--min-count", type=int, default=1)
    args = parser.parse_args(argv)
    build(args.source, pathlib.Path(args.output), args.min_elo, args.min_opponent,
          args.min_base_seconds, args.max_ply, args.min_count)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
