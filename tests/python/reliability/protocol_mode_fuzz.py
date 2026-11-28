#!/usr/bin/env python3
"""Adversarial multi-protocol and CLI fuzz for Koi.

Exercises CLI flags (--version, --engine-json, --protocol, invalid flags),
XBoard CECP v2, USI, UCCI, UCI_Cyclone, mid-search protocol switching,
hostile numeric/string tokens, CRLF/NUL framing, and healthy teardown.

    python tests/python/reliability/protocol_mode_fuzz.py \
        --engine build/ci-release/koi.exe --cases 100 \
        --workers 2 --out build/ci-release/reliability/protocol_mode-fuzz.json
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

PROTOCOLS = ["uci", "usi", "ucci", "ucicyclone", "xboard", "XBOARD"]

UCI_LIKE_OPS = [
    "uci",
    "usi",
    "ucci",
    "isready",
    "ucinewgame",
    "usinewgame",
    "uccinewgame",
    "setoption name Protocol value uci",
    "setoption name Protocol value usi",
    "setoption name Protocol value ucci",
    "setoption name Protocol value ucicyclone",
    "setoption name Protocol value xboard",
    "setoption name Protocol value nonsense",
    "setoption name OwnBook value false",
    "setoption OwnBook false",
    "position startpos",
    "position startpos moves e2e4 e7e5 g1f3 b8c6",
    "position fen 7k/6Q1/6K1/8/8/8/8/8 b - - 0 1",
    "position fen malformed",
    "go depth 1",
    "go time 10",
    "go nodes 16",
    "go infinite",
    "go ponder depth 1",
    "ponderhit",
    "stop",
    "annotate e2e4 depth 0",
    "annotate e2e4 movetime -1",
    "annotate e2e4 infinite",
    "annotate game depth 1",
    "bogus_command_token",
]

XBOARD_OPS = [
    "xboard",
    "protover 2",
    "protover -1",
    "new",
    "force",
    "go",
    "playother",
    "analyze",
    "exit",
    "hard",
    "easy",
    "post",
    "nopost",
    "?",
    "ping 1",
    "ping abc",
    "st nan",
    "st -nan",
    "st inf",
    "st -inf",
    "st 0",
    "st 0.01",
    "level 0 1:30 2",
    "level 40 5 nan",
    "level 40 5 inf",
    "level 4294967297 5 0",
    "time 9223372036854775807",
    "otim -9223372036854775808",
    "time 5000",
    "otim 5000",
    "sd 1",
    "sd 0",
    "sd 99",
    "variant suicide",
    "variant normal",
    "usermove e2e4",
    "usermove e9e9",
    "d7d5",
    "setboard malformed",
    "setboard 7k/5Q2/6K1/8/8/8/8/8 b - - 0 1",
    "setboard rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -",
    "undo",
    "remove",
    "cores 9999",
    "memory 999999999",
    "option Threads=9999",
    "option Hash=999999999",
    "option EvalFile=does-not-exist-xboard.nnue",
    "option SyzygyPath=missing-syzygy-xboard",
    "option SyzygyPath=",
    "option OwnBook=false",
    "option BogusOption=1",
    "perft 1",
    "annotate e2e4 depth 1",
    "position fen malformed",
]

MOVE_RE = r"[a-h][1-8][a-h][1-8][nbrq]?"
XBOARD_PURE_VALID_PATTERNS = [
    re.compile(r"^feature .+$"),
    re.compile(r"^pong \S+$"),
    re.compile(rf"^move {MOVE_RE}$"),
    re.compile(r"^(?:1-0|0-1|1/2-1/2) \{.+\}$"),
    re.compile(r"^Error \(.+\): .+$"),
    re.compile(r"^Illegal move: .+$"),
    re.compile(r"^tellusererror .+$"),
    re.compile(rf"^\d+\s+-?\d+\s+\d+\s+\d+(?:\s+{MOVE_RE})*$"),
]

VALID_STDOUT_PATTERNS = [
    re.compile(r"^id (?:name|author) .+$"),
    re.compile(r"^option name .+$"),
    re.compile(r"^(?:uciok|usiok|ucciok|readyok|nobestmove)$"),
    re.compile(r"^info .+$"),
    re.compile(rf"^bestmove (?:{MOVE_RE}|0000|resign)(?: ponder (?:{MOVE_RE}|0000))?$"),
] + XBOARD_PURE_VALID_PATTERNS


def _is_valid_protocol_line(line: str) -> bool:
    return any(pattern.fullmatch(line) is not None for pattern in VALID_STDOUT_PATTERNS)


def _is_valid_xboard_line(line: str) -> bool:
    return any(pattern.fullmatch(line) is not None for pattern in XBOARD_PURE_VALID_PATTERNS)


def _run_cli_contract_case(engine: str, case: int) -> dict:
    result = {
        "case": case,
        "mode": "cli",
        "exit": None,
        "timeout": False,
        "bad_stderr": [],
        "bad_stdout": [],
    }
    env = os.environ.copy()
    try:
        if case == 0:
            proc = subprocess.run(
                [engine, "--version"],
                input=b"",
                capture_output=True,
                env=env,
                timeout=15,
            )
            result["exit"] = proc.returncode
            stdout = proc.stdout.decode("utf-8", errors="replace").strip()
            stderr = proc.stderr.decode("utf-8", errors="replace")
            if proc.returncode != 0:
                result["bad_stderr"].append(f"expected exit 0 for --version, got {proc.returncode}")
            if not re.fullmatch(r"Koi \S+ \([^)]+\)", stdout):
                result["bad_stdout"].append(f"unexpected --version output: {stdout!r}")
            if stderr:
                result["bad_stderr"].append(f"unexpected stderr on --version: {stderr!r}")
        elif case == 1:
            proc = subprocess.run(
                [engine, "--engine-json"],
                input=b"",
                capture_output=True,
                env=env,
                timeout=15,
            )
            result["exit"] = proc.returncode
            stdout = proc.stdout.decode("utf-8", errors="replace")
            stderr = proc.stderr.decode("utf-8", errors="replace")
            if proc.returncode != 0:
                result["bad_stderr"].append(f"expected exit 0 for --engine-json, got {proc.returncode}")
            try:
                payload = json.loads(stdout)
                if not isinstance(payload, dict) or "name" not in payload:
                    result["bad_stdout"].append(f"incomplete --engine-json payload: {payload!r}")
            except json.JSONDecodeError as exc:
                result["bad_stdout"].append(f"invalid --engine-json JSON: {exc}")
            if stderr:
                result["bad_stderr"].append(f"unexpected stderr on --engine-json: {stderr!r}")
        elif case == 6:
            if sys.platform == "win32":
                helper = (
                    "import ctypes, subprocess, sys\n"
                    "ctypes.windll.kernel32.SetStdHandle(-12, ctypes.c_void_p(0))\n"
                    "p = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, "
                    "stdout=subprocess.PIPE, stderr=None, creationflags=0x08000008)\n"
                    "out, _ = p.communicate(b'uci\\nisready\\nposition startpos\\ngo depth 2\\nstop\\nquit\\n', timeout=20)\n"
                    "sys.stdout.buffer.write(out)\n"
                    "sys.exit(p.returncode)\n"
                )
                proc = subprocess.run(
                    [sys.executable, "-c", helper, engine],
                    input=b"",
                    capture_output=True,
                    env=env,
                    timeout=25,
                )
            else:
                proc = subprocess.run(
                    [engine],
                    input=b"uci\nisready\nposition startpos\ngo depth 2\nstop\nquit\n",
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    env=env,
                    timeout=25,
                )
            result["exit"] = proc.returncode
            stdout = proc.stdout.decode("utf-8", errors="replace")
            stderr = proc.stderr.decode("utf-8", errors="replace") if proc.stderr else ""
            if proc.returncode != 0:
                result["bad_stderr"].append(f"expected exit 0 with unavailable stderr, got {proc.returncode}")
            if stderr:
                result["bad_stderr"].append(f"unexpected stderr in child helper: {stderr!r}")
            if "uciok" not in stdout or "bestmove " not in stdout:
                result["bad_stdout"].append(f"missing uciok/bestmove with unavailable stderr: {stdout!r}")
        else:
            cli_cases = {
                2: ([engine, "--protocol"], 2, "missing argument for --protocol"),
                3: ([engine, "--protocol", "invalid_proto"], 2, "unknown protocol"),
                4: ([engine, "--protocol=shogi"], 2, "unknown protocol"),
                5: ([engine, "--bogus-cli-flag"], 2, "unknown option"),
            }
            argv, expected_rc, expected_err = cli_cases[case]
            proc = subprocess.run(argv, input=b"", capture_output=True, env=env, timeout=15)
            result["exit"] = proc.returncode
            stdout = proc.stdout.decode("utf-8", errors="replace")
            stderr = proc.stderr.decode("utf-8", errors="replace")
            if proc.returncode != expected_rc:
                result["bad_stderr"].append(f"expected exit {expected_rc}, got {proc.returncode}")
            if stdout:
                result["bad_stdout"].append(f"expected empty stdout on CLI error, got {stdout!r}")
            if expected_err not in stderr:
                result["bad_stderr"].append(f"expected {expected_err!r} in stderr, got {stderr!r}")
    except subprocess.TimeoutExpired:
        result["timeout"] = True
        result["bad_stderr"].append("timeout")
    return result


def run_case(config: dict) -> dict:
    case = int(config["case"])
    engine = str(config["engine"])
    if case < 7:
        return _run_cli_contract_case(engine, case)

    rng = random.Random(20261121 + case)
    end_mode = case % 5
    pure_xboard = end_mode == 4
    start_proto = rng.choice(["xboard", "XBOARD"]) if pure_xboard else PROTOCOLS[(case - 7) % len(PROTOCOLS)]
    argv = [engine]
    flag_style = rng.randrange(3)
    env = os.environ.copy()
    if flag_style == 0:
        argv.extend(["--protocol", start_proto])
    elif flag_style == 1:
        argv.append(f"--protocol={start_proto}")
    else:
        env["KOI_PROTOCOL"] = start_proto

    body: list[bytes] = []
    if start_proto.lower() == "xboard":
        body.append(b"protover 2")
        body.append(b"option OwnBook=false")

    op_count = rng.randint(6, 18)
    for step in range(op_count):
        if pure_xboard:
            pool = XBOARD_OPS
        else:
            pool = XBOARD_OPS if rng.random() < 0.5 else UCI_LIKE_OPS
        cmd = rng.choice(pool).encode("utf-8")
        if rng.random() < 0.12:
            cmd = b"\x00" + cmd
        if rng.random() < 0.08:
            cmd = cmd + b" " + (b"x" * rng.randint(64, 512))
        body.append(cmd)
        if not pure_xboard and step % 5 == 4:
            # Ensure cross-protocol switches happen cleanly in between storms.
            body.append(rng.choice([b"uci", b"usi", b"ucci", b"xboard\nprotover 2"]))

    if end_mode == 0:
        tail = [
            b"uci",
            b"setoption name OwnBook value false",
            b"isready",
            b"position startpos",
            b"go depth 1",
            b"stop",
            b"quit",
        ]
    elif end_mode == 1:
        tail = [
            b"usi",
            b"setoption name OwnBook value false",
            b"isready",
            b"position startpos",
            b"go depth 1",
            b"stop",
            b"quit",
        ]
    elif end_mode == 2:
        tail = [
            b"ucci",
            b"setoption OwnBook false",
            b"isready",
            b"position startpos",
            b"go time 25",
            b"stop",
            b"bye",
        ]
    elif end_mode == 3:
        tail = [
            b"uci",
            b"setoption name Protocol value ucicyclone",
            b"ucci",
            b"setoption name OwnBook value false",
            b"isready",
            b"position startpos",
            b"go depth 1",
            b"stop",
            b"quit",
        ]
    else:
        tail = [
            b"new",
            b"option OwnBook=false",
            b"sd 1",
            b"go",
            b"ping 9999",
            b"quit",
        ]

    lines_bytes = body + tail
    payload_parts = [
        line + (b"\r\n" if rng.random() < 0.35 else b"\n") for line in lines_bytes
    ]
    payload = b"".join(payload_parts)

    result = {
        "case": case,
        "mode": "xboard_pure" if pure_xboard else f"{start_proto}->end_{end_mode}",
        "exit": None,
        "timeout": False,
        "bad_stderr": [],
        "bad_stdout": [],
    }
    try:
        proc = subprocess.run(
            argv,
            input=payload,
            capture_output=True,
            env=env,
            timeout=25,
        )
        result["exit"] = proc.returncode
        stdout = proc.stdout.decode("utf-8", errors="replace")
        stderr = proc.stderr.decode("utf-8", errors="replace")
        if proc.returncode != 0:
            result["bad_stderr"].append(f"exit {proc.returncode}")
        for err_line in stderr.splitlines():
            if err_line and not err_line.startswith("UCI position error:"):
                result["bad_stderr"].append(err_line)

        out_lines = [line.rstrip("\r") for line in stdout.splitlines()]
        validator = _is_valid_xboard_line if pure_xboard else _is_valid_protocol_line
        for out_line in out_lines:
            if out_line and not validator(out_line):
                result["bad_stdout"].append(f"invalid protocol line: {out_line!r}")

        if end_mode == 4:
            if not any(line.startswith("move ") for line in out_lines):
                result["bad_stdout"].append("missing CECP move reply after final XBoard go")
            if "pong 9999" not in out_lines:
                result["bad_stdout"].append("missing final pong 9999 after XBoard go")
        else:
            if not any(line.startswith("bestmove ") for line in out_lines):
                result["bad_stdout"].append("missing bestmove reply after final search")
    except subprocess.TimeoutExpired:
        result["timeout"] = True
        result["bad_stderr"].append("timeout")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cases", type=int, default=100)
    parser.add_argument("--workers", type=int, default=min(4, os.cpu_count() or 2))
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    engine = os.path.abspath(args.engine)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    configs = [{"case": case, "engine": engine} for case in range(args.cases)]
    results: list[dict] = []
    step = max(1, min(25, args.cases // 5))
    started = time.monotonic()

    with mp.Pool(processes=max(1, args.workers)) as pool:
        for done, result in enumerate(pool.imap_unordered(run_case, configs), 1):
            results.append(result)
            if result["timeout"] or result["bad_stderr"] or result["bad_stdout"]:
                print(
                    f"FAIL case {result['case']} ({result['mode']}): "
                    f"stderr={result['bad_stderr'][:2]} stdout={result['bad_stdout'][:2]}",
                    flush=True,
                )
            if done % step == 0 or done == args.cases:
                pct = (done * 100) // max(1, args.cases)
                eta = format_progress_eta(done, args.cases, time.monotonic() - started)
                print(f"progress {done}/{args.cases} ({pct}%) | {eta}", flush=True)

    summary = {
        "engine": engine,
        "cases": args.cases,
        "timeouts": sum(1 for r in results if r["timeout"]),
        "unexpected_exits": sum(
            1 for r in results if r["bad_stderr"] and any("expected exit" in e or e.startswith("exit ") for e in r["bad_stderr"])
        ),
        "unexpected_stderr": sum(1 for r in results if r["bad_stderr"]),
        "bad_stdout": sum(1 for r in results if r["bad_stdout"]),
        "results_file": str(out_path),
    }
    out_path.write_text(
        json.dumps({"summary": summary, "results": results}, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(summary, indent=2))
    return (
        0
        if not (
            summary["timeouts"]
            or summary["unexpected_exits"]
            or summary["unexpected_stderr"]
            or summary["bad_stdout"]
        )
        else 2
    )


if __name__ == "__main__":
    sys.exit(main())
