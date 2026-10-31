#!/usr/bin/env python3
"""CCRL readiness: 1000 ultra-fast games without crashes or UCI violations.

Each game starts two fresh Koi processes, plays fixed 5 ms/move from one of the
curated openings, validates every move with python-chess, checks every stdout
line against the UCI grammar, requires empty stderr and a clean exit, and
records the outcome.  Run with:

    python artifacts/plans/ccrl-eligibility-2026-11-01/crash_1000.py \
        --engine build/full-release-gpu/koi-engine.exe \
        --games 1000 --workers 6 --movetime 5 \
        --out artifacts/plans/ccrl-eligibility-2026-11-01/1000-games.json

Exit code 0 means every game finished with zero crashes, timeouts, illegal
moves or protocol violations.
"""

from __future__ import annotations

import argparse
import json
import multiprocessing as mp
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

import chess

OPTION_RE = re.compile(r"^option name .+ type (check|spin|combo|button|string)( .*)?$")
BESTMOVE_RE = re.compile(
    r"^bestmove ([a-h][1-8][a-h][1-8][nbrq]?|0000)( ponder [a-h][1-8][a-h][1-8][nbrq]?)?$"
)
ALLOWED_LINE_RE = re.compile(
    r"^(id (name|author) .+|option name .+|uciok|readyok|info .+|bestmove .+)$"
)


class ProtocolError(RuntimeError):
    pass


class Engine:
    def __init__(self, executable: str, strict: bool = True) -> None:
        env = os.environ.copy()
        self.process = subprocess.Popen(
            [executable],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            env=env,
            cwd=os.path.dirname(os.path.abspath(executable)),
        )
        self.lines: list[str] = []
        self.stderr_lines: list[str] = []
        self.protocol_errors: list[str] = []
        self._cursor = 0
        self._strict = strict
        self._lock = threading.Lock()
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def _read_stdout(self) -> None:
        assert self.process.stdout is not None
        for raw in self.process.stdout:
            line = raw.rstrip("\r\n")
            if not line:
                continue
            if self._strict and not ALLOWED_LINE_RE.match(line):
                with self._lock:
                    self.protocol_errors.append(line)
            with self._lock:
                self.lines.append(line)

    def _read_stderr(self) -> None:
        assert self.process.stderr is not None
        for raw in self.process.stderr:
            line = raw.rstrip("\r\n")
            if line:
                with self._lock:
                    self.stderr_lines.append(line)

    def send(self, command: str) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

    def wait_for(self, predicate, timeout_s: float, label: str) -> str:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            with self._lock:
                # Lines are consumed monotonically: a later wait must never
                # re-match an earlier bestmove/readyok from a previous command.
                snapshot = self.lines[self._cursor:]
                self._cursor = len(self.lines)
                protocol_errors = list(self.protocol_errors)
            if protocol_errors:
                raise ProtocolError(f"{label}: non-UCI output {protocol_errors[0]!r}")
            for line in snapshot:
                if predicate(line):
                    return line
            if self.process.poll() is not None:
                raise RuntimeError(
                    f"{label}: engine exited with {self.process.returncode}"
                )
            time.sleep(0.002)
        raise TimeoutError(f"{label}: no expected output within {timeout_s}s")

    def shutdown(self, timeout_s: float = 5.0) -> tuple[int | None, list[str], list[str]]:
        try:
            if self.process.poll() is None:
                self.send("quit")
        except Exception:
            pass
        try:
            self.process.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            return (None, ["did not exit within timeout"], [])
        with self._lock:
            return (
                self.process.returncode,
                list(self.stderr_lines),
                list(self.protocol_errors),
            )


def load_openings(path: str) -> list[tuple[str, list[chess.Move]]]:
    openings: list[tuple[str, list[chess.Move]]] = []
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        name, _, moves_text = line.partition("|")
        if not moves_text:
            name, moves_text = "opening", line
        try:
            board = chess.Board()
            moves = []
            for token in moves_text.split():
                move = chess.Move.from_uci(token)
                if move not in board.legal_moves:
                    raise ValueError(token)
                board.push(move)
                moves.append(move)
        except Exception:
            continue
        openings.append((name.strip(), moves))
    if not openings:
        raise SystemExit(f"no usable openings in {path}")
    return openings


def configure_engine(engine: Engine, is_koi: bool) -> None:
    engine.send("uci")
    engine.wait_for(lambda line: line == "uciok", 20.0, "handshake")
    options = [
        ("Hash", "256"),
        ("Threads", "1"),
        ("MultiPV", "1"),
        ("SyzygyPath", ""),
    ]
    if is_koi:
        # CCRL: internal book and pondering off (explicitly sent even though
        # the defaults are already off, so the tested configuration is explicit).
        options.extend([("OwnBook", "false"), ("Ponder", "false")])
    for option, value in options:
        engine.send(f"setoption name {option} value {value}")
    engine.send("isready")
    engine.wait_for(lambda line: line == "readyok", 20.0, "readiness")


def play_game(config: dict) -> dict:
    game_id = config["game_id"]
    opening_name, opening_moves = config["openings"][game_id % len(config["openings"])]
    board = chess.Board()
    for move in opening_moves:
        board.push(move)
    koi = Engine(config["engine"], strict=True)
    if config.get("opponent"):
        opponent = Engine(config["opponent"], strict=False)
        opponent_is_koi = False
    else:
        # Self-play still runs two independent processes, but both are Koi so
        # every observed error counts toward eligibility.
        opponent = Engine(config["engine"], strict=True)
        opponent_is_koi = True
    koi_white = bool(config.get("koi_white", True))
    engines = [koi, opponent] if koi_white else [opponent, koi]
    is_koi = [(engine is koi) or opponent_is_koi for engine in engines]
    koi_errors: list[str] = []
    opponent_errors: list[str] = []
    started = time.time()
    plies = 0
    try:
        setup_failed = False
        for index, engine in enumerate(engines):
            errors_for_side = koi_errors if is_koi[index] else opponent_errors
            try:
                configure_engine(engine, is_koi[index])
            except Exception as exc:  # noqa: BLE001 - recorded per engine
                errors_for_side.append(f"setup: {type(exc).__name__}: {exc}")
                setup_failed = True
                break
        if setup_failed:
            raise RuntimeError("engine setup failed")
        for engine in engines:
            engine.send("position fen " + board.fen())
        while plies < config["max_plies"] and not board.is_game_over():
            moving_index = 0 if board.turn == chess.WHITE else 1
            engine = engines[moving_index]
            errors = koi_errors if is_koi[moving_index] else opponent_errors
            engine.send(f"go movetime {config['movetime_ms']}")
            try:
                line = engine.wait_for(
                    lambda candidate: candidate.startswith("bestmove "),
                    config["move_timeout_s"],
                    f"game {game_id} ply {plies} bestmove",
                )
            except Exception as exc:  # noqa: BLE001 - recorded per engine
                errors.append(f"{type(exc).__name__}: {exc}")
                break
            match = BESTMOVE_RE.match(line)
            if not match:
                errors.append(f"invalid bestmove line: {line}")
                break
            token = match.group(1)
            if token == "0000":
                errors.append("bestmove 0000 while a legal move exists")
                break
            try:
                move = chess.Move.from_uci(token)
            except ValueError:
                errors.append(f"unparsable bestmove: {token}")
                break
            if move not in board.legal_moves:
                errors.append(f"illegal bestmove {token} in {board.fen()}")
                break
            board.push(move)
            plies += 1
            for engine_instance in engines:
                engine_instance.send("position fen " + board.fen())
        if board.is_checkmate():
            outcome = "checkmate"
            result = "0-1" if board.turn == chess.WHITE else "1-0"
        elif board.is_stalemate():
            outcome = "stalemate"
            result = "1/2-1/2"
        elif board.is_insufficient_material():
            outcome = "insufficient-material"
            result = "1/2-1/2"
        elif board.is_seventyfive_moves():
            outcome = "seventyfive-moves"
            result = "1/2-1/2"
        elif board.is_fivefold_repetition():
            outcome = "fivefold"
            result = "1/2-1/2"
        elif plies >= config["max_plies"]:
            outcome = "max-plies"
            result = "*"
        else:
            outcome = "other"
            result = "*"
    except Exception as exc:  # noqa: BLE001 - unexpected harness/game failure
        outcome = "error"
        result = "*"
        if not (koi_errors or opponent_errors) or str(exc) != "engine setup failed":
            koi_errors.append(f"{type(exc).__name__}: {exc}")
    finally:
        for index, engine in enumerate(engines):
            errors = koi_errors if is_koi[index] else opponent_errors
            code, err_lines, proto = engine.shutdown()
            if code not in (0, None):
                errors.append(f"non-zero engine exit code: {code}")
            if err_lines:
                errors.append(f"stderr output: {err_lines[:3]}")
            if proto:
                errors.append(f"protocol violations: {proto[:3]}")
    return {
        "game": game_id,
        "opening": opening_name,
        "koi_white": koi_white,
        "plies": plies,
        "outcome": outcome,
        "result": result,
        "errors": koi_errors,
        "opponent_errors": opponent_errors,
        "seconds": round(time.time() - started, 3),
    }


def _worker(config: dict) -> dict:
    return play_game(config)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True)
    parser.add_argument("--games", type=int, default=1000)
    parser.add_argument("--workers", type=int, default=min(6, os.cpu_count() or 2))
    parser.add_argument("--movetime", dest="movetime_ms", type=int, default=5)
    parser.add_argument("--max-plies", type=int, default=300)
    parser.add_argument("--move-timeout", dest="move_timeout_s", type=float, default=10.0)
    parser.add_argument(
        "--openings",
        default="tests/data/openings/openings-curated-32.txt",
    )
    parser.add_argument(
        "--opponent",
        default="",
        help="Optional second engine (e.g. Stockfish 19) to diversify games; "
        "empty means Koi self-play.",
    )
    parser.add_argument("--out", required=True)
    parser.add_argument("--seed", type=int, default=20261101)
    args = parser.parse_args()

    engine = str(Path(args.engine).resolve())
    if not Path(engine).is_file():
        raise SystemExit(f"engine not found: {engine}")
    opponent = str(Path(args.opponent).resolve()) if args.opponent else ""
    if opponent and not Path(opponent).is_file():
        raise SystemExit(f"opponent not found: {opponent}")
    openings = load_openings(args.openings)
    configs = [
        {
            "game_id": game_id,
            "engine": engine,
            "openings": openings,
            "movetime_ms": args.movetime_ms,
            "max_plies": args.max_plies,
            "move_timeout_s": args.move_timeout_s,
            "seed": args.seed,
            "opponent": opponent,
            "koi_white": (game_id % 2 == 0) if opponent else True,
        }
        for game_id in range(args.games)
    ]

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    jsonl_path = out_path.with_suffix(".jsonl")
    results: list[dict] = []
    started = time.time()
    failures = 0
    with jsonl_path.open("w", encoding="utf-8") as stream:
        with mp.Pool(processes=max(1, args.workers)) as pool:
            for done, result in enumerate(pool.imap_unordered(_worker, configs), 1):
                results.append(result)
                stream.write(json.dumps(result) + "\n")
                stream.flush()
                if result["errors"]:
                    failures += 1
                    print(
                        f"FAIL game {result['game']} after {result['plies']} plies: "
                        f"{result['errors'][:2]}",
                        flush=True,
                    )
                if done % 50 == 0 or done == args.games:
                    elapsed = time.time() - started
                    rate = done / elapsed if elapsed else 0
                    print(
                        f"progress {done}/{args.games} games "
                        f"({rate:.2f} games/s, failures={failures})",
                        flush=True,
                    )
    total_seconds = round(time.time() - started, 2)
    summary = {
        "engine": engine,
        "games": args.games,
        "workers": args.workers,
        "movetime_ms": args.movetime_ms,
        "max_plies": args.max_plies,
        "openings_file": str(Path(args.openings).resolve()),
        "total_seconds": total_seconds,
        "games_with_errors": failures,
        "crashes": sum(
            1 for r in results if any("non-zero engine exit" in e or "stderr" in e for e in r["errors"])
        ),
        "timeouts": sum(1 for r in results if any("Timeout" in e for e in r["errors"])),
        "illegal_moves": sum(1 for r in results if any("illegal bestmove" in e for e in r["errors"])),
        "protocol_violations": sum(
            1 for r in results if any("protocol violation" in e or "non-UCI" in e for e in r["errors"])
        ),
        "opponent_games_with_errors": sum(1 for r in results if r["opponent_errors"]),
        "opponent_error_samples": [
            r["opponent_errors"][:2] for r in results if r["opponent_errors"]
        ][:3],
        "outcomes": {
            outcome: sum(1 for r in results if r["outcome"] == outcome)
            for outcome in sorted({r["outcome"] for r in results})
        },
        "results_jsonl": str(jsonl_path),
    }
    out_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0 if failures == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
