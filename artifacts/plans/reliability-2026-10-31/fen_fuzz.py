#!/usr/bin/env python3
"""Adversarial FEN parser fuzz for Koi.

Feeds mutated/random FEN-like lines to a real UCI session after a known-good
position.  Expected outcomes: exit 0, no hang, at most position-error diagnostics
on stderr, and exactly one legal bestmove from the final healthy startpos search.
Wrongly accepted FENs are fine as long as the engine stays safe.

    python artifacts/plans/reliability-2026-10-31/fen_fuzz.py \
        --engine build/full-release-gpu/koi-engine.exe --cases 2000 \
        --workers 6 --out artifacts/plans/reliability-2026-10-31/fen-fuzz.json
"""

from __future__ import annotations

import argparse
import json
import multiprocessing as mp
import os
import random
import re
import subprocess
import pathlib
import sys

PIECES = "pnbrqkPNBRQK"
BOARD_CHARS = PIECES + "12345678"
SIDE = ["w", "b", "x", "", "white"]
CASTLE = ["KQkq", "K", "k", "-", "KQk", "ABCD", "", "KQkqKQkq"]
EP = ["-", "e3", "e6", "a1", "z9", "", "e9", "a3"]


def random_board(rng: random.Random) -> str:
    style = rng.randrange(6)
    if style == 0:
        # Plausible-looking but random piece placement.
        rows = []
        for _ in range(8):
            row = []
            empty = 8
            while empty > 0:
                if rng.random() < 0.55:
                    row.append(rng.choice(PIECES))
                    empty -= 1
                else:
                    count = rng.randint(1, min(8, empty))
                    row.append(str(count))
                    empty -= count
            rows.append("".join(row))
        return "/".join(rows)
    if style == 1:
        # Valid startpos with random mutations.
        board = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR"
        for _ in range(rng.randint(1, 6)):
            index = rng.randrange(len(board))
            board = board[:index] + rng.choice(BOARD_CHARS + "/") + board[index + 1 :]
        return board
    if style == 2:
        return "/".join(
            "".join(rng.choice(BOARD_CHARS) for _ in range(rng.randint(0, 12)))
            for _ in range(rng.randint(0, 10))
        )
    if style == 3:
        return "".join(rng.choice(string_guard()) for _ in range(rng.randint(0, 80)))
    if style == 4:
        return "/".join(
            rng.choice(["8", "8/8", "k7", "K7", "3k4", "3K4", "4k3", "4K3"])
            for _ in range(rng.randint(1, 8))
        )
    return rng.choice(
        [
            "",
            " ",
            "8/8/8/8/8/8/8/8",
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR",
            "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R",
            "\u265a\u265b\u265c\u265d\u265e\u265f",
        ]
    )


def string_guard() -> str:
    return PIECES + "12345678/- wKQkqabcdefgh0"


FIXTURE_FENS: list[str] = []


def _load_fixture_lines(path: pathlib.Path) -> list[str]:
    try:
        return [line.strip() for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    except OSError:
        return []


def make_valid_fen(rng: random.Random) -> str:
    if not FIXTURE_FENS:
        return "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
    return rng.choice(FIXTURE_FENS)


def make_fen(rng: random.Random) -> str:
    if rng.random() < 0.5:
        return make_valid_fen(rng)
    board = random_board(rng)
    side = rng.choice(SIDE)
    castle = rng.choice(CASTLE)
    ep = rng.choice(EP)
    halfmove = rng.choice(["0", "1", "-1", "999999999", "abc", "", "65536"])
    fullmove = rng.choice(["1", "0", "-4", "999999999", "x", ""])
    separator = rng.choice([" ", "  ", "\t"])
    fields = [board, side, castle, ep, halfmove, fullmove]
    return separator.join(fields)


def run_case(config: dict) -> dict:
    rng = random.Random(20261101 + config["case"])
    fen = make_fen(rng)
    # Newlines/NULs would split the command; keep one line.
    fen = fen.replace("\n", " ").replace("\r", " ").replace("\x00", " ")
    engine = config["engine"]
    env = os.environ.copy()
    transcript = (
        "uci\nisready\n"
        "position startpos\n"
        f"position fen {fen}\n"
        "go depth 1\nstop\nquit\n"
    )
    result = {
        "case": config["case"],
        "fen": fen,
        "exit": None,
        "timeout": False,
        "bestmoves": [],
        "bad_stderr": [],
        "bad_stdout": [],
        "position_error": False,
    }
    try:
        proc = subprocess.run(
            [engine],
            input=transcript,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=env,
            timeout=15,
        )
        result["exit"] = proc.returncode
        stdout = proc.stdout
        stderr = proc.stderr
        if proc.returncode != 0:
            result["bad_stderr"].append(f"exit {proc.returncode}")
        result["bestmoves"] = [
            line for line in stdout.splitlines() if line.startswith("bestmove ")
        ]
        if len(result["bestmoves"]) != 1:
            result["bad_stdout"].append(
                f"expected exactly one bestmove, got {result['bestmoves']}"
            )
        if result["bestmoves"] and not re.fullmatch(
            r"bestmove ([a-h][1-8][a-h][1-8][nbrq]?|0000)( ponder [a-h][1-8][a-h][1-8][nbrq]?)?",
            result["bestmoves"][0],
        ):
            result["bad_stdout"].append(f"invalid bestmove: {result['bestmoves'][0]}")
        for line in stderr.splitlines():
            if line.startswith("UCI position error:"):
                result["position_error"] = True
            else:
                result["bad_stderr"].append(line)
    except subprocess.TimeoutExpired:
        result["timeout"] = True
        result["bad_stderr"].append("timeout")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cases", type=int, default=2000)
    parser.add_argument("--workers", type=int, default=min(6, os.cpu_count() or 2))
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    engine = os.path.abspath(args.engine)
    configs = [{"case": case, "engine": engine} for case in range(args.cases)]
    results: list[dict] = []
    with mp.Pool(processes=max(1, args.workers)) as pool:
        for done, result in enumerate(pool.imap_unordered(run_case, configs), 1):
            results.append(result)
            if result["timeout"] or result["bad_stderr"] or result["bad_stdout"]:
                print(
                    f"FAIL case {result['case']}: fen={result['fen']!r} "
                    f"stderr={result['bad_stderr'][:2]} stdout={result['bad_stdout'][:2]}",
                    flush=True,
                )
            if done % 200 == 0 or done == args.cases:
                print(f"progress {done}/{args.cases}", flush=True)
    summary = {
        "engine": engine,
        "cases": args.cases,
        "timeouts": sum(1 for r in results if r["timeout"]),
        "nonzero_exits": sum(1 for r in results if r["exit"] not in (0, None)),
        "unexpected_stderr": sum(1 for r in results if r["bad_stderr"]),
        "bad_stdout": sum(1 for r in results if r["bad_stdout"]),
        "accepted_without_position_error": sum(
            1 for r in results if not r["position_error"] and r["exit"] == 0 and r["bestmoves"]
        ),
        "results_file": args.out,
    }
    with open(args.out, "w", encoding="utf-8") as stream:
        json.dump({"summary": summary, "results": results}, stream, indent=2)
    print(json.dumps(summary, indent=2))
    return 0 if not (summary["timeouts"] or summary["nonzero_exits"] or summary["unexpected_stderr"] or summary["bad_stdout"]) else 2


if __name__ == "__main__":
    FIXTURE_FENS = _load_fixture_lines(
        pathlib.Path(__file__).resolve().parents[3] / "tests/data/reliability/valid-fens.txt"
    )
    sys.exit(main())
