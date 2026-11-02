#!/usr/bin/env python3
"""Adversarial `setoption` parser fuzz for Koi.

Sends random option commands (valid names with hostile values, malformed
setoption syntax, control bytes, quotes, long strings) before a final healthy
search.  Hash/Threads are intentionally excluded (their valid numeric values can
allocate aggressively); the goal is parser/state robustness, not resource bombs.

    python artifacts/plans/reliability-2026-10-31/option_fuzz.py \
        --engine build/full-release/koi-engine.exe --cases 1000 \
        --workers 6 --out artifacts/plans/reliability-2026-10-31/option-fuzz.json
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

SAFE_NAMES = [
    "BookFile",
    "DebugFile",
    "EvalFile",
    "SyzygyPath",
    "OwnBook",
    "BookRandom",
    "BookSafety",
    "BookDepth",
    "BookSafetyDepth",
    "RandomSeed",
    "Contempt",
    "Move Overhead",
    "Slow Mover",
    "MultiPV",
    "Ponder",
    "UCI_AnalyseMode",
    "Debug",
    "SyzygyProbeLimit",
    "SyzygyProbeDepth",
    "SyzygyInteriorDepth",
    "NoSuchOption",
]
SYNTAX = [
    "setoption",
    "setoption name",
    "setoption name BookFile",
    "setoption name BookFile value",
    "setoption value x",
    "setoption name  value x",
    "SetOption Name OwnBook Value true",
    " setoption  name  OwnBook  value  maybe ",
]
SPECIAL_VALUES = [
    "",
    " ",
    '""',
    "C:\\Program Files\\koi book.bin",
    "\\\\server\\share\\book.bin",
    ";;;",
    ":::",
    "e2e4",
    "true",
    "false",
    "maybe",
    "123456789012345678901234567890",
    "-1",
    "0",
    "1",
    "\x00",
    "\x01\x02\x7f",
    "\u00fc\u4e2d\u6587",
    "a" * 4096,
    "a" * 16384,
]


def random_name(rng: random.Random) -> str:
    style = rng.randrange(5)
    if style < 3:
        return rng.choice(SAFE_NAMES)
    if style == 3:
        return "".join(rng.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz0123456789-")
                       for _ in range(rng.randint(0, 40)))
    return rng.choice(["Hash", "Threads", "Hash ", " Threads"])


def random_value(rng: random.Random) -> str:
    style = rng.randrange(4)
    if style < 2:
        return rng.choice(SPECIAL_VALUES)
    if style == 2:
        return "".join(
            rng.choice("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 \\/;:'\"._-")
            for _ in range(rng.randint(0, 60))
        )
    return str(rng.randint(-1000, 100000))


def make_command(rng: random.Random) -> str:
    if rng.random() < 0.15:
        return rng.choice(SYNTAX)
    name = random_name(rng)
    value = random_value(rng)
    base = name.strip().lower()
    if base == "hash":
        # Never send a valid multi-hundred-MB Hash request from a fuzzer.
        value = rng.choice(["maybe", "-1", "0", "999999", "abc", ""])
    elif base == "threads":
        value = rng.choice(["1", "2", "maybe", "-1", "99999"])
    if rng.random() < 0.1:
        return f"setoption name {name}"
    return f"setoption name {name} value {value}"


def run_case(config: dict) -> dict:
    rng = random.Random(20261101 + config["case"])
    commands = [make_command(rng) for _ in range(rng.randint(5, 40))]
    transcript_lines = ["uci", "isready"]
    for command in commands:
        transcript_lines.append(command)
        transcript_lines.append("isready")
    transcript_lines += ["position startpos", "go depth 1", "stop", "quit"]
    transcript = "\n".join(transcript_lines) + "\n"
    engine = config["engine"]
    env = os.environ.copy()
    result = {
        "case": config["case"],
        "commands": commands[:12],
        "exit": None,
        "timeout": False,
        "bestmoves": [],
        "bad_stderr": [],
        "bad_stdout": [],
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
            timeout=20,
        )
        result["exit"] = proc.returncode
        stdout = proc.stdout
        stderr = proc.stderr
        if proc.returncode != 0:
            result["bad_stderr"].append(f"exit {proc.returncode}")
        result["bestmoves"] = [line for line in stdout.splitlines() if line.startswith("bestmove ")]
        if len(result["bestmoves"]) != 1:
            result["bad_stdout"].append(
                f"expected exactly one bestmove, got {result['bestmoves']}"
            )
        if result["bestmoves"] and not re.fullmatch(
            r"bestmove ([a-h][1-8][a-h][1-8][nbrq]?|0000)( ponder [a-h][1-8][a-h][1-8][nbrq]?)?",
            result["bestmoves"][0],
        ):
            result["bad_stdout"].append(f"invalid bestmove: {result['bestmoves'][0]}")
        for line in stdout.splitlines():
            if line and not (
                line.startswith("id ")
                or line.startswith("option name ")
                or line.startswith("info ")
                or line in ("uciok", "readyok")
                or line.startswith("bestmove ")
            ):
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
    with mp.Pool(processes=max(1, args.workers)) as pool:
        for done, result in enumerate(pool.imap_unordered(run_case, configs), 1):
            results.append(result)
            if result["timeout"] or result["bad_stderr"] or result["bad_stdout"]:
                print(
                    f"FAIL case {result['case']}: commands={result['commands']!r} "
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
    sys.exit(main())
