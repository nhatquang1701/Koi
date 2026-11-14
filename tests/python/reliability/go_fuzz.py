#!/usr/bin/env python3
"""Adversarial `go`/state-machine fuzz for Koi.

Feeds random search-limit combinations and interleaved lifecycle commands, then
a deterministic healthy search and checks crash/hang/protocol invariants.

    python artifacts/plans/reliability-2026-10-31/go_fuzz.py \
        --engine build/full-release/koi-engine.exe --cases 1000 \
        --workers 6 --out artifacts/plans/reliability-2026-10-31/go-fuzz.json
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
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "support"))
from unittest_progress import format_progress_eta  # noqa: E402

GO_TOKENS = [
    "depth", "nodes", "mate", "movetime", "wtime", "btime", "winc", "binc",
    "movestogo", "infinite", "ponder", "searchmoves",
]
VALUES = ["0", "1", "-1", "5", "10", "200", "1000", "30000", "999999999999999999999", "abc", "", "2147483647"]
LIFECYCLE = ["isready", "ucinewgame", "stop", "ponderhit", "setoption name MultiPV value 2",
             "setoption name MultiPV value 1", "setoption name Ponder value true",
             "setoption name UCI_AnalyseMode value true", "setoption name UCI_AnalyseMode value false"]
MOVES = ["e2e4", "e7e5", "g1f3", "b8c6", "a2a3", "h7h6"]


def random_go(rng: random.Random) -> str:
    style = rng.randrange(5)
    if style == 0:
        return "go"
    if style == 1:
        return rng.choice(["go infinite", "go ponder", "go depth 1", "go movetime 5"])
    tokens: list[str] = []
    for _ in range(rng.randint(1, 5)):
        token = rng.choice(GO_TOKENS)
        if token == "infinite":
            tokens.append("infinite")
        elif token == "ponder":
            tokens.append("ponder")
        elif token == "searchmoves":
            tokens.append("searchmoves")
            tokens.extend(rng.sample(MOVES, rng.randint(0, 3)))
        else:
            tokens.append(f"{token} {rng.choice(VALUES)}")
    return "go " + " ".join(tokens)


FIXTURE_FENS: list[str] = []


def _load_fixture_lines(path: Path) -> list[str]:
    try:
        return [line.strip() for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    except OSError:
        return []


def random_position(rng: random.Random) -> str:
    if rng.random() < 0.5 or not FIXTURE_FENS:
        return "position startpos"
    return f"position fen {rng.choice(FIXTURE_FENS)}"


def run_case(config: dict) -> dict:
    rng = random.Random(20261101 + config["case"])
    lines = ["uci", "isready"]
    for _ in range(rng.randint(3, 15)):
        kind = rng.randrange(10)
        if kind < 6:
            lines.append(random_go(rng))
        elif kind < 8:
            lines.append(random_position(rng))
        else:
            lines.append(rng.choice(LIFECYCLE))
    # Deterministic healthy search for the completion invariant.
    lines += ["position startpos", "go depth 1", "stop", "quit"]
    transcript = "\n".join(lines) + "\n"
    engine = config["engine"]
    env = os.environ.copy()
    result = {"case": config["case"], "lines": lines[:14], "exit": None,
              "timeout": False, "bestmoves": [], "bad_stderr": [], "bad_stdout": []}
    try:
        proc = subprocess.run([engine], input=transcript, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", env=env, timeout=25)
        result["exit"] = proc.returncode
        stdout, stderr = proc.stdout, proc.stderr
        if proc.returncode != 0:
            result["bad_stderr"].append(f"exit {proc.returncode}")
        result["bestmoves"] = [line for line in stdout.splitlines() if line.startswith("bestmove ")]
        if not result["bestmoves"]:
            result["bad_stdout"].append("expected at least one bestmove")
        if result["bestmoves"] and not re.fullmatch(
            r"bestmove ([a-h][1-8][a-h][1-8][nbrq]?|0000)( ponder [a-h][1-8][a-h][1-8][nbrq]?)?",
            result["bestmoves"][0]):
            result["bad_stdout"].append(f"invalid bestmove: {result['bestmoves'][0]}")
        for line in stdout.splitlines():
            if line and not (
                line.startswith("id ") or line.startswith("option name ")
                or line.startswith("info ") or line in ("uciok", "readyok")
                or line.startswith("bestmove ")):
                result["bad_stdout"].append(f"non-UCI stdout: {line!r}")
        for line in stderr.splitlines():
            if line and not line.startswith("UCI position error:"):
                result["bad_stderr"].append(line)
    except subprocess.TimeoutExpired:
        result["timeout"] = True
        result["bad_stderr"].append("timeout")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cases", type=int, default=1000)
    parser.add_argument("--workers", type=int, default=min(6, os.cpu_count() or 2))
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    engine = os.path.abspath(args.engine)
    configs = [{"case": case, "engine": engine} for case in range(args.cases)]
    results: list[dict] = []
    step = max(1, min(25, args.cases // 5))
    started = time.monotonic()
    with mp.Pool(processes=max(1, args.workers)) as pool:
        for done, result in enumerate(pool.imap_unordered(run_case, configs), 1):
            results.append(result)
            if result["timeout"] or result["bad_stderr"] or result["bad_stdout"]:
                print(f"FAIL case {result['case']}: lines={result['lines']!r} "
                      f"stderr={result['bad_stderr'][:2]} stdout={result['bad_stdout'][:2]}", flush=True)
            if done % step == 0 or done == args.cases:
                pct = (done * 100) // max(1, args.cases)
                eta = format_progress_eta(done, args.cases, time.monotonic() - started)
                print(f"progress {done}/{args.cases} ({pct}%) | {eta}", flush=True)
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
    FIXTURE_FENS = _load_fixture_lines(
        Path(__file__).resolve().parents[3] / "tests/data/reliability/valid-fens.txt"
    )
    sys.exit(main())
