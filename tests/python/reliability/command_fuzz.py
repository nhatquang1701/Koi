#!/usr/bin/env python3
"""Randomized raw UCI-command-line fuzz for Koi.

Sends arbitrary command lines (unknown commands, control characters, random
tokens and fragments) followed by a deterministic healthy search.  Unknown
commands must be inert; stdout must stay protocol-valid; stderr may only contain
documented position errors; at least one legal bestmove must complete.

    python artifacts/plans/reliability-2026-10-31/command_fuzz.py \
        --engine build/full-release-gpu/koi-engine.exe --cases 500 \
        --workers 4 --out artifacts/plans/reliability-2026-10-31/command-fuzz.json
"""

from __future__ import annotations

import argparse
import json
import multiprocessing as mp
import os
import random
import re
import subprocess
import sys

FRAGMENTS = [
    "uci", "isready", "ucinewgame", "stop", "ponderhit", "go", "position",
    "setoption", "debug", "register", "d", "eval", "board", "help", "quitx",
    "setoption name", "position startpos", "go depth", "setoption name Hash value",
]
CHARS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
PUNCT = " \t/\\;:'\"{}[]()<>|&*#@!$%^~`+-=_,.?"


def random_command(rng: random.Random) -> str:
    style = rng.randrange(6)
    if style == 0:
        return rng.choice(FRAGMENTS)
    if style == 1:
        return "".join(rng.choice(CHARS + PUNCT) for _ in range(rng.randint(0, 120)))
    if style == 2:
        return rng.choice(FRAGMENTS) + " " + "".join(
            rng.choice(CHARS + PUNCT) for _ in range(rng.randint(0, 40))
        )
    if style == 3:
        return "".join(
            rng.choice(CHARS + PUNCT + "\x01\x02\x7f")
            for _ in range(rng.randint(1, 60))
        )
    if style == 4:
        return rng.choice(["0", "-1", "999999999999999999999", "nan", "true", "false"])
    return ""


def run_case(config: dict) -> dict:
    rng = random.Random(20261101 + config["case"])
    lines = ["uci", "isready"]
    for _ in range(rng.randint(4, 24)):
        command = random_command(rng)
        # Never let random input close the session before the healthy tail.
        if command.strip().lower() == "quit":
            command = "stop"
        lines.append(command)
    lines += ["position startpos", "go depth 1", "stop", "quit"]
    transcript = "\n".join(lines) + "\n"
    result = {"case": config["case"], "lines": lines[:12], "exit": None,
              "timeout": False, "bestmoves": [], "bad_stderr": [], "bad_stdout": []}
    try:
        proc = subprocess.run(
            [config["engine"]], input=transcript, capture_output=True, text=True,
            encoding="utf-8", errors="replace", env=os.environ, timeout=20,
        )
        result["exit"] = proc.returncode
        if proc.returncode != 0:
            result["bad_stderr"].append(f"exit {proc.returncode}")
        stdout = proc.stdout
        result["bestmoves"] = [line for line in stdout.splitlines() if line.startswith("bestmove ")]
        if not result["bestmoves"]:
            result["bad_stdout"].append("expected at least one bestmove")
        for move in result["bestmoves"]:
            if not re.fullmatch(
                r"bestmove ([a-h][1-8][a-h][1-8][nbrq]?|0000)( ponder [a-h][1-8][a-h][1-8][nbrq]?)?",
                move,
            ):
                result["bad_stdout"].append(f"invalid bestmove: {move!r}")
        for line in stdout.splitlines():
            if line and not (
                line.startswith("id ")
                or line.startswith("option name ")
                or line.startswith("info ")
                or line in ("uciok", "readyok")
                or line.startswith("bestmove ")
            ):
                result["bad_stdout"].append(f"non-UCI stdout: {line!r}")
        for line in proc.stderr.splitlines():
            if line and not line.startswith("UCI position error:"):
                result["bad_stderr"].append(line)
    except subprocess.TimeoutExpired:
        result["timeout"] = True
        result["bad_stderr"].append("timeout")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cases", type=int, default=500)
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
                print(f"FAIL case {result['case']}: lines={result['lines']!r} "
                      f"stderr={result['bad_stderr'][:2]} stdout={result['bad_stdout'][:2]}", flush=True)
            if done % 100 == 0 or done == args.cases:
                print(f"progress {done}/{args.cases}", flush=True)
    summary = {
        "engine": engine,
        "cases": args.cases,
        "timeouts": sum(1 for r in results if r["timeout"]),
        "nonzero_exits": sum(1 for r in results if r["exit"] not in (0, None)),
        "unexpected_stderr": sum(1 for r in results if r["bad_stderr"]),
        "bad_stdout": sum(1 for r in results if r["bad_stdout"]),
        "results_file": args.out,
    }
    with open(args.out, "w", encoding="utf-8") as stream:
        json.dump({"summary": summary, "results": results}, stream, indent=2)
    print(json.dumps(summary, indent=2))
    return 0 if not (summary["timeouts"] or summary["nonzero_exits"]
                     or summary["unexpected_stderr"] or summary["bad_stdout"]) else 2


if __name__ == "__main__":
    sys.exit(main())
