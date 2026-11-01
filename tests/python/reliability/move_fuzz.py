#!/usr/bin/env python3
"""Adversarial UCI move-list fuzz for Koi.

Feeds mutated move sequences to `position startpos moves ...` and verifies the
engine never crashes or hangs: exit 0, stderr only `UCI position error:` lines,
and exactly one legal bestmove from the retained position.

    python artifacts/plans/reliability-2026-10-31/move_fuzz.py \
        --engine build/full-release-gpu/koi-engine.exe --cases 2000 \
        --workers 6 --out artifacts/plans/reliability-2026-10-31/move-fuzz.json
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

TOKEN_CHARS = "abcdefgh12345678nbrqk"


def random_token(rng: random.Random) -> str:
    style = rng.randrange(6)
    if style == 0:
        return "".join(rng.choice(TOKEN_CHARS) for _ in range(rng.randint(0, 8)))
    if style == 1:
        return rng.choice(
            ["e2e4", "e7e5", "g1f3", "b8c6", "e1g1", "e1h1", "e7e8q", "e7e8", "0000", "--", "null"]
        )
    if style == 2:
        return "e2e4\x01"
    if style == 3:
        return rng.choice(["E2E4", "e2E4", "e2e9", "i2i4", "e2e4q", "e2e4k"])
    if style == 4:
        return " ".join(rng.choice(TOKEN_CHARS) for _ in range(rng.randint(1, 4)))
    return rng.choice(["", " ", "\t"])


FIXTURE_MOVE_LISTS: list[list[str]] = []


def _load_fixture_lines(path: pathlib.Path) -> list[str]:
    try:
        return [line.strip() for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    except OSError:
        return []


def random_game_moves(rng: random.Random) -> list[str]:
    if not FIXTURE_MOVE_LISTS:
        return [random_token(rng) for _ in range(rng.randint(1, 20))]
    return list(rng.choice(FIXTURE_MOVE_LISTS))


def make_moves(rng: random.Random) -> list[str]:
    style = rng.randrange(6)
    if style == 0:
        return random_game_moves(rng)
    if style == 1:
        moves = random_game_moves(rng)
        if moves:
            index = rng.randrange(len(moves))
            moves[index] = random_token(rng)
        return moves
    if style == 2:
        return [random_token(rng) for _ in range(rng.randint(0, 40))]
    if style == 3:
        return random_game_moves(rng)[: rng.randint(0, 30)] + [
            random_token(rng) for _ in range(rng.randint(0, 10))
        ]
    if style == 4:
        # Chess960-style castling tokens and odd promotions.
        return ["e1h1", "e1a1", "e8h8", "e8a8", "e7e8q", "e7e8n", "e2e4"]
    return [random_token(rng) for _ in range(rng.randint(200, 1500))]


def run_case(config: dict) -> dict:
    rng = random.Random(20261101 + config["case"])
    moves = make_moves(rng)
    move_text = " ".join(moves).strip()
    engine = config["engine"]
    env = os.environ.copy()
    transcript = (
        "uci\nisready\n"
        f"position startpos moves {move_text}\n"
        "go depth 1\nstop\nquit\n"
    )
    result = {
        "case": config["case"],
        "moves": move_text[:400],
        "token_count": len(moves),
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
                    f"FAIL case {result['case']}: moves={result['moves']!r} "
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
        "max_tokens": max((r["token_count"] for r in results), default=0),
        "results_file": args.out,
    }
    with open(args.out, "w", encoding="utf-8") as stream:
        json.dump({"summary": summary, "results": results}, stream, indent=2)
    print(json.dumps(summary, indent=2))
    return 0 if not (
        summary["timeouts"]
        or summary["nonzero_exits"]
        or summary["unexpected_stderr"]
        or summary["bad_stdout"]
    ) else 2


if __name__ == "__main__":
    FIXTURE_MOVE_LISTS = [
        line.split()
        for line in _load_fixture_lines(
            pathlib.Path(__file__).resolve().parents[3]
            / "tests/data/reliability/valid-move-lists.txt"
        )
    ]
    sys.exit(main())
