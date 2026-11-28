"""Build a compact Stockfish-verified CC0 Polyglot opening book for Koi.

Combines strong CC0 Lichess game trees with Stockfish analysis, cycle-free DAG
minimax backpropagation, and soundness filtering so dubious traps never receive
high weights under deterministic ``BookRandom=false`` selection while compact
branch variety fits comfortably inside release archives.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import glob
import hashlib
import heapq
import io
import json
import math
import pathlib
import re
import subprocess
import sys
from collections import deque
from typing import Iterable

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
for extra_path in (REPO_ROOT / "tools" / "book", REPO_ROOT / "tools" / "measurement"):
    if str(extra_path) not in sys.path:
        sys.path.insert(0, str(extra_path))

import koi_chess as chess
import polyglot

try:
    import zstandard
except ImportError:  # pragma: no cover - optional wheel
    zstandard = None

HEADER_RE = re.compile(r'^\[(\w+) "(.*)"\]\r?$')
COMMENT_RE = re.compile(r"\{[^}]*\}")
VARIATION_RE = re.compile(r"\([^()]*\)")
MOVE_NUMBER_RE = re.compile(r"\d+\.(?:\.\.)?")
RESULTS = {"1-0": 2, "1/2-1/2": 1, "0-1": 0}
MAX_WEIGHT = 65535


@dataclasses.dataclass
class EdgeStats:
    """Statistics and graph link for one candidate move from a node."""

    uci: str
    encoded_move: int
    child_key: int
    count: int = 0
    stm_points_x2: int = 0
    sf_seeded: bool = False


@dataclasses.dataclass
class EngineEval:
    """Stockfish evaluation relative to the side to move at a position."""

    eval_cp: int
    best_move: str | None = None


@dataclasses.dataclass
class CandidateDAG:
    """Transposition-merged opening graph extracted from game corpora and seeds."""

    root_key: int
    fens: dict[int, str]
    min_ply: dict[int, int]
    edges: dict[int, dict[str, EdgeStats]]


def _parse_base_seconds(time_control: str) -> int:
    if not time_control or time_control == "-":
        return 0
    base = time_control.split("+")[0]
    if "/" in base:
        base = base.split("/")[-1]
    try:
        return int(base)
    except ValueError:
        return 0


def qualify_headers(
    headers: dict[str, str],
    min_elo: float,
    min_opponent: int,
    min_base_seconds: int,
) -> int | None:
    """Return White's result doubled (2, 1, 0) if the game qualifies, else None."""
    result_code = RESULTS.get(headers.get("Result", ""))
    if result_code is None:
        return None
    try:
        white = int(headers.get("WhiteElo", "0") or 0)
        black = int(headers.get("BlackElo", "0") or 0)
    except ValueError:
        return None
    if white < min_opponent or black < min_opponent:
        return None
    if (white + black) / 2.0 < min_elo:
        return None
    if _parse_base_seconds(headers.get("TimeControl", "")) < min_base_seconds:
        return None
    if headers.get("Variant", "Standard") not in ("Standard", "standard"):
        return None
    return result_code


def _open_pgn_lines(path: pathlib.Path) -> Iterable[str]:
    if path.suffix == ".zst":
        if zstandard is not None:
            with path.open("rb") as raw:
                reader = zstandard.ZstdDecompressor().stream_reader(raw)
                text_stream = io.TextIOWrapper(reader, encoding="utf-8", errors="replace")
                yield from text_stream
            return
        proc = subprocess.Popen(
            ["zstd", "-dc", str(path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        assert proc.stdout is not None
        try:
            yield from proc.stdout
        finally:
            proc.stdout.close()
            if proc.wait() != 0 and proc.stderr is not None:
                err = proc.stderr.read()
                raise RuntimeError(f"zstd failed for {path}: {err}")
            if proc.stderr is not None:
                proc.stderr.close()
        return

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        yield from handle


def _strip_movetext(lines: list[str]) -> list[str]:
    text = " ".join(lines)
    text = COMMENT_RE.sub(" ", text)
    while True:
        stripped = VARIATION_RE.sub(" ", text)
        if stripped == text:
            break
        text = stripped
    text = MOVE_NUMBER_RE.sub(" ", text)
    return [tok for tok in text.split() if tok and tok not in ("1-0", "0-1", "1/2-1/2", "*")]


def _record_line(
    dag: CandidateDAG,
    moves_iter: Iterable[tuple[chess.Board, chess.Move]],
    white_points_x2: int,
    sf_seeded: bool,
) -> None:
    parent_key = dag.root_key
    for ply, (board_before, move) in enumerate(moves_iter):
        uci = move.uci()
        encoded = polyglot.encode_move(board_before, move)
        board_before.push(move)
        child_key = polyglot.polyglot_key(board_before)
        if child_key not in dag.fens:
            dag.fens[child_key] = board_before.fen()
            dag.min_ply[child_key] = ply + 1
        elif ply + 1 < dag.min_ply[child_key]:
            dag.min_ply[child_key] = ply + 1

        node_edges = dag.edges.setdefault(parent_key, {})
        stats = node_edges.get(uci)
        if stats is None:
            stats = EdgeStats(
                uci=uci,
                encoded_move=encoded,
                child_key=child_key,
                sf_seeded=sf_seeded,
            )
            node_edges[uci] = stats
        stats.count += 1
        stm_white = not board_before.turn  # turn flipped after push
        stats.stm_points_x2 += white_points_x2 if stm_white else (2 - white_points_x2)
        parent_key = child_key


def _iter_san_moves(tokens: list[str], max_ply: int):
    board = chess.Board()
    for ply, token in enumerate(tokens):
        if ply >= max_ply:
            return
        try:
            move = board.parse_san(token)
        except ValueError:
            return
        yield board, move


def _load_seed_openings(dag: CandidateDAG, seed_paths: list[str], max_ply: int) -> int:
    loaded = 0
    for pattern in seed_paths:
        for file_str in sorted(glob.glob(str(pathlib.Path(pattern).expanduser()))):
            path = pathlib.Path(file_str)
            with path.open("r", encoding="utf-8", errors="replace") as handle:
                for raw in handle:
                    line = raw.strip()
                    if not line or line.startswith("#"):
                        continue
                    board = chess.Board()
                    seq: list[chess.Move] = []
                    parts = line.split("\t")
                    text = parts[-1] if len(parts) >= 3 else line
                    lowered = text.lower()
                    if lowered.startswith("eco") or lowered == "pgn":
                        continue
                    tokens = _strip_movetext([text])
                    for ply, tok in enumerate(tokens):
                        if ply >= max_ply:
                            break
                        is_uci = (
                            len(tok) in (4, 5)
                            and tok[0] in "abcdefgh"
                            and tok[1] in "12345678"
                            and tok[2] in "abcdefgh"
                            and tok[3] in "12345678"
                        )
                        try:
                            mv = chess.Move.from_uci(tok) if is_uci else board.parse_san(tok)
                            if mv not in board.legal_moves:
                                mv = board.parse_san(tok)
                        except ValueError:
                            seq = []
                            break
                        seq.append(mv)
                        board.push(mv)
                    if seq:
                        # Reset and replay cleanly onto a fresh board so _record_line pushes in order.
                        fresh = chess.Board()
                        def _replay(moves_list=seq, b=fresh):
                            for mv in moves_list:
                                yield b, mv
                        _record_line(dag, _replay(), white_points_x2=1, sf_seeded=True)
                        loaded += 1
    return loaded


def _max_branch_for_ply(ply: int) -> int:
    if ply <= 2:
        return 10
    if ply <= 5:
        return 6
    if ply <= 10:
        return 4
    return 3


def recompute_dag_plies(dag: CandidateDAG) -> None:
    """Compute shortest BFS ply from root_key over current edges."""
    dist: dict[int, int] = {dag.root_key: 0}
    queue: deque[int] = deque([dag.root_key])
    while queue:
        u = queue.popleft()
        d = dist[u] + 1
        for edge in dag.edges.get(u, {}).values():
            v = edge.child_key
            if v not in dist:
                dist[v] = d
                queue.append(v)
    dag.min_ply = dist


def compact_candidate_dag(
    dag: CandidateDAG,
    min_count: int,
    max_nodes: int,
    max_ply: int,
) -> CandidateDAG:
    """Keep a cycle-free, mainline-prioritized DAG up to ``max_nodes`` positions."""
    recompute_dag_plies(dag)
    kept_edges: dict[int, dict[str, EdgeStats]] = {}
    kept_fens: dict[int, str] = {dag.root_key: dag.fens[dag.root_key]}
    kept_ply: dict[int, int] = {dag.root_key: 0}
    expanded: set[int] = set()
    # Priority: (tier 0 for ply<=3, -effective_score, orig_ply, tie_uci, parent, edge)
    heap: list[tuple[int, int, int, str, int, EdgeStats]] = []

    def _enqueue_node(u: int) -> None:
        if u in expanded:
            return
        expanded.add(u)
        u_ply = dag.min_ply.get(u, max_ply)
        if u_ply >= max_ply:
            return
        candidates = []
        for edge in dag.edges.get(u, {}).values():
            v = edge.child_key
            orig_v_ply = dag.min_ply.get(v)
            if orig_v_ply is None or orig_v_ply <= u_ply:
                continue
            threshold = 1 if edge.sf_seeded else (2 if u_ply <= 4 else min_count)
            if edge.count < threshold:
                continue
            candidates.append(edge)
        candidates.sort(key=lambda e: (-int(e.sf_seeded), -e.count, e.uci))
        for edge in candidates[: _max_branch_for_ply(u_ply)]:
            tier = 0 if u_ply <= 3 else 1
            eff_score = edge.count + (50 if edge.sf_seeded else 0)
            sort_ply = u_ply if tier == 0 else 0
            heapq.heappush(heap, (tier, -eff_score, sort_ply, edge.uci, u, edge))

    _enqueue_node(dag.root_key)
    while heap:
        _, _, _, _, u, edge = heapq.heappop(heap)
        u_ply = dag.min_ply[u]
        if len(kept_edges.get(u, {})) >= _max_branch_for_ply(u_ply):
            continue
        v = edge.child_key
        if v not in kept_fens:
            if len(kept_fens) >= max_nodes:
                continue
            kept_fens[v] = dag.fens[v]
            kept_ply[v] = u_ply + 1
        elif dag.min_ply[v] <= u_ply:
            continue
        kept_edges.setdefault(u, {})[edge.uci] = edge
        _enqueue_node(v)

    result = CandidateDAG(dag.root_key, kept_fens, kept_ply, kept_edges)
    recompute_dag_plies(result)
    return result


def extract_candidate_tree(
    sources: list[str],
    seed_openings: list[str],
    min_elo: float,
    min_opponent: int,
    min_base_seconds: int,
    max_ply: int,
    min_count: int,
    max_nodes: int,
    max_games: int = 0,
) -> CandidateDAG:
    """Parse CC0 Lichess PGNs and seed lines into a compact candidate DAG."""
    start = chess.Board()
    root_key = polyglot.polyglot_key(start)
    dag = CandidateDAG(
        root_key=root_key,
        fens={root_key: start.fen()},
        min_ply={root_key: 0},
        edges={},
    )
    if seed_openings:
        seeded = _load_seed_openings(dag, seed_openings, max_ply)
        print(f"seeded {seeded} opening lines", flush=True)

    games_seen = qualified = 0
    for pattern in sources:
        for file_str in sorted(glob.glob(str(pathlib.Path(pattern).expanduser()))):
            headers: dict[str, str] = {}
            movetext: list[str] = []
            state = "headers"
            pending_result: int | None = None
            for raw in _open_pgn_lines(pathlib.Path(file_str)):
                line = raw.rstrip("\r\n")
                if state == "headers":
                    if line.startswith("["):
                        match = HEADER_RE.match(line)
                        if match:
                            headers[match.group(1)] = match.group(2)
                        continue
                    if line == "":
                        games_seen += 1
                        pending_result = qualify_headers(
                            headers, min_elo, min_opponent, min_base_seconds
                        )
                        if pending_result is not None:
                            state = "body"
                            movetext = []
                            qualified += 1
                        else:
                            state = "skip"
                    continue
                if state == "body":
                    if line == "":
                        assert pending_result is not None
                        tokens = _strip_movetext(movetext)
                        _record_line(dag, _iter_san_moves(tokens, max_ply), pending_result, False)
                        headers, movetext, state, pending_result = {}, [], "headers", None
                        if max_games and qualified >= max_games:
                            break
                    else:
                        movetext.append(line)
                    continue
                if line == "":
                    headers, state = {}, "headers"
            if state == "body" and pending_result is not None:
                tokens = _strip_movetext(movetext)
                _record_line(dag, _iter_san_moves(tokens, max_ply), pending_result, False)
            print(
                f"scanned {file_str}: games={games_seen} qualified={qualified} "
                f"raw_nodes={len(dag.fens)}",
                flush=True,
            )
            if max_games and qualified >= max_games:
                break

    compacted = compact_candidate_dag(dag, min_count=min_count, max_nodes=max_nodes, max_ply=max_ply)
    print(f"compacted candidate DAG to {len(compacted.fens)} positions", flush=True)
    return compacted


def _parse_scored_info(line: str) -> tuple[int, str | None, bool] | None:
    """Extract (eval_cp, pv0, exact_bound) from a UCI info line containing ``score``."""
    tokens = line.split()
    if not tokens or tokens[0] != "info" or "score" not in tokens:
        return None
    cp_val: int | None = None
    pv0: str | None = None
    is_bound = "upperbound" in tokens or "lowerbound" in tokens
    idx = 1
    while idx < len(tokens):
        tok = tokens[idx]
        if tok == "score" and idx + 2 < len(tokens):
            kind = tokens[idx + 1]
            raw_score = tokens[idx + 2]
            try:
                num = int(raw_score)
            except ValueError:
                return None
            if kind == "cp":
                cp_val = num
            elif kind == "mate":
                cp_val = (10000 - num) if num > 0 else (-10000 - num)
            idx += 3
        elif tok == "pv" and idx + 1 < len(tokens):
            cand = tokens[idx + 1]
            if cand not in ("0000", "(none)", "none"):
                pv0 = cand
            break
        else:
            idx += 1
    if cp_val is None:
        return None
    return cp_val, pv0, is_bound


def _eval_worker_chunk(
    worker_args: tuple[str, list[tuple[int, str]], int, int, int]
) -> list[tuple[int, int, str | None]]:
    engine_path, items, nodes_limit, depth_limit, hash_mb = worker_args
    proc = subprocess.Popen(
        [engine_path],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )
    assert proc.stdin is not None and proc.stdout is not None

    def send(cmd: str) -> None:
        assert proc.stdin is not None
        proc.stdin.write(cmd + "\n")
        proc.stdin.flush()

    def wait_for(token: str) -> None:
        assert proc.stdout is not None
        for raw in proc.stdout:
            if raw.strip() == token:
                return
        raise RuntimeError(f"UCI engine exited before {token}")

    results: list[tuple[int, int, str | None]] = []
    try:
        send("uci")
        wait_for("uciok")
        send(f"setoption name Hash value {hash_mb}")
        send("setoption name Threads value 1")
        send("isready")
        wait_for("readyok")
        go_cmd = f"go depth {depth_limit}" if depth_limit > 0 else f"go nodes {nodes_limit}"
        for key, fen in items:
            send(f"position fen {fen}")
            send(go_cmd)
            best_eval = 0
            best_pv0: str | None = None
            has_exact = False
            bestmove_tok: str | None = None
            for raw in proc.stdout:
                stripped = raw.strip()
                if stripped.startswith("bestmove "):
                    parts = stripped.split()
                    if len(parts) > 1 and parts[1] not in ("0000", "(none)", "none"):
                        bestmove_tok = parts[1]
                    break
                parsed = _parse_scored_info(stripped)
                if parsed is not None:
                    val, pv0, is_bound = parsed
                    if not is_bound or not has_exact:
                        best_eval = val
                        if pv0 is not None:
                            best_pv0 = pv0
                        if not is_bound:
                            has_exact = True
            results.append((key, best_eval, best_pv0 or bestmove_tok))
        send("quit")
        proc.wait(timeout=5)
    finally:
        for stream in (proc.stdin, proc.stdout, proc.stderr):
            try:
                if stream is not None:
                    stream.close()
            except OSError:
                pass
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    return results


def _load_eval_cache(cache_path: pathlib.Path | None) -> dict[int, EngineEval]:
    cached: dict[int, EngineEval] = {}
    if cache_path is None or not cache_path.exists():
        return cached
    with cache_path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            cached[int(row["key"], 16)] = EngineEval(
                eval_cp=int(row["eval_cp"]),
                best_move=row.get("best_move"),
            )
    return cached


def _evaluate_missing(
    items: list[tuple[int, str]],
    evals: dict[int, EngineEval],
    engine_path: str,
    workers: int,
    nodes_limit: int,
    depth_limit: int,
    hash_mb: int,
    cache_path: pathlib.Path | None,
) -> None:
    pending = [(k, fen) for k, fen in items if k not in evals]
    if not pending:
        return
    print(f"evaluating {len(pending)} positions with {workers} worker(s)...", flush=True)
    chunk_size = max(1, min(256, math.ceil(len(pending) / max(1, workers * 4))))
    chunks = [pending[i : i + chunk_size] for i in range(0, len(pending), chunk_size)]
    cache_handle = None
    if cache_path is not None:
        cache_path.parent.mkdir(parents=True, exist_ok=True)
        cache_handle = cache_path.open("a", encoding="utf-8")
    try:
        completed = 0
        worker_jobs = [(engine_path, chunk, nodes_limit, depth_limit, hash_mb) for chunk in chunks]
        if workers <= 1:
            stream = (_eval_worker_chunk(job) for job in worker_jobs)
        else:
            pool = concurrent.futures.ProcessPoolExecutor(max_workers=workers)
            stream = pool.map(_eval_worker_chunk, worker_jobs)
        try:
            for chunk_out in stream:
                for key, cp, best_mv in chunk_out:
                    ev = EngineEval(eval_cp=cp, best_move=best_mv)
                    evals[key] = ev
                    if cache_handle is not None:
                        cache_handle.write(
                            json.dumps({"key": hex(key), "eval_cp": cp, "best_move": best_mv}) + "\n"
                        )
                if cache_handle is not None:
                    cache_handle.flush()
                completed += len(chunk_out)
                if completed % 2000 < len(chunk_out) or completed == len(pending):
                    print(f"evaluated {completed}/{len(pending)}", flush=True)
        finally:
            if workers > 1:
                pool.shutdown(wait=True)
    finally:
        if cache_handle is not None:
            cache_handle.close()


def evaluate_and_expand_with_engine(
    dag: CandidateDAG,
    engine_path: str,
    workers: int,
    nodes_limit: int,
    depth_limit: int,
    hash_mb: int,
    expand_ply: int,
    max_nodes: int,
    cache_path: pathlib.Path | None,
) -> dict[int, EngineEval]:
    """Evaluate all DAG positions and optionally inject top Stockfish replies."""
    evals = _load_eval_cache(cache_path)
    items = sorted(dag.fens.items(), key=lambda kv: (dag.min_ply.get(kv[0], 999), kv[0]))
    _evaluate_missing(items, evals, engine_path, workers, nodes_limit, depth_limit, hash_mb, cache_path)

    for ply_pass in range(expand_ply):
        added: list[tuple[int, str]] = []
        for u, u_ply in list(dag.min_ply.items()):
            if u_ply != ply_pass:
                continue
            ev = evals.get(u)
            if ev is None or not ev.best_move:
                continue
            node_edges = dag.edges.setdefault(u, {})
            if ev.best_move in node_edges:
                continue
            board = chess.Board(dag.fens[u])
            try:
                mv = chess.Move.from_uci(ev.best_move)
            except ValueError:
                continue
            if mv not in board.legal_moves:
                continue
            encoded = polyglot.encode_move(board, mv)
            board.push(mv)
            child_key = polyglot.polyglot_key(board)
            if child_key in dag.min_ply and dag.min_ply[child_key] <= u_ply:
                continue
            if child_key not in dag.fens:
                if len(dag.fens) >= max_nodes + 30_000:
                    continue
                child_fen = board.fen()
                dag.fens[child_key] = child_fen
                dag.min_ply[child_key] = u_ply + 1
                if child_key not in evals:
                    added.append((child_key, child_fen))
            node_edges[ev.best_move] = EdgeStats(
                uci=ev.best_move,
                encoded_move=encoded,
                child_key=child_key,
                count=0,
                stm_points_x2=0,
                sf_seeded=True,
            )
        if added:
            print(f"expand ply {ply_pass}: injecting {len(added)} Stockfish top lines", flush=True)
            _evaluate_missing(
                added, evals, engine_path, workers, nodes_limit, depth_limit, hash_mb, cache_path
            )
    recompute_dag_plies(dag)
    return evals


def _max_kept_moves_for_ply(ply: int) -> int:
    if ply <= 1:
        return 5
    if ply <= 5:
        return 4
    if ply <= 11:
        return 3
    return 2


def minimax_and_prune(
    dag: CandidateDAG,
    evals: dict[int, EngineEval],
    tolerance_cp: int = 25,
    min_abs_cp: int = -65,
    max_size_bytes: int = 2_000_000,
) -> list[tuple[int, int, int, int]]:
    """Backpropagate minimax scores over forward DAG edges and pack Polyglot records."""
    recompute_dag_plies(dag)
    v_node: dict[int, int] = {k: evals[k].eval_cp for k in dag.fens if k in evals}
    max_d = max((dag.min_ply.get(k, 0) for k in dag.fens), default=0)

    by_ply: dict[int, list[int]] = {}
    for k, d in dag.min_ply.items():
        by_ply.setdefault(d, []).append(k)

    # Backpropagate only across strictly deeper child plies so cycles are impossible.
    kept_edge_scores: dict[int, list[tuple[EdgeStats, int]]] = {}
    for d in range(max_d, -1, -1):
        for u in by_ply.get(d, []):
            static_u = v_node.get(u)
            if static_u is None:
                continue
            scored: list[tuple[EdgeStats, int]] = []
            for edge in dag.edges.get(u, {}).values():
                v = edge.child_key
                if dag.min_ply.get(v, -1) <= d or v not in v_node:
                    continue
                scored.append((edge, -v_node[v]))
            if not scored:
                continue
            best_child_val = max(sc for _, sc in scored)
            # If every seeded branch drops >45 cp below Stockfish's own static eval
            # at u, exit book cleanly at u instead of lowering u or keeping bad moves.
            if best_child_val < static_u - 45:
                continue
            v_node[u] = min(static_u, best_child_val)
            kept_edge_scores[u] = scored

    # Select sound moves and compute deterministic + weighted-random weights per node.
    selected: dict[int, list[tuple[EdgeStats, int, int]]] = {}
    for u, scored in kept_edge_scores.items():
        u_ply = dag.min_ply[u]
        best_eval = max(sc for _, sc in scored)
        floor = max(min_abs_cp, best_eval - tolerance_cp)
        passing = [(edge, sc) for edge, sc in scored if sc >= floor]
        if not passing and best_eval >= min_abs_cp - 20:
            passing = [max(scored, key=lambda pair: (pair[1], pair[0].count))]
        if not passing:
            continue

        passing.sort(key=lambda pair: (-pair[1], -pair[0].count, pair[0].uci))
        passing = passing[: _max_kept_moves_for_ply(u_ply)]

        best_uci = passing[0][0].uci
        weighted: list[tuple[EdgeStats, int, int]] = []
        raw_weights: list[tuple[int, float]] = []
        for idx, (edge, sc) in enumerate(passing):
            diff = max(0, best_eval - sc)
            eval_factor = math.exp(-diff / 12.0)
            perf = (edge.stm_points_x2 + 12.0) / (2.0 * edge.count + 24.0) if edge.count > 0 else 0.55
            pop = math.log1p(edge.count) / math.log1p(500.0) if edge.count > 0 else 0.45
            score_w = 10000.0 * eval_factor * (0.70 + 0.30 * perf) * (0.75 + 0.25 * min(1.0, pop))
            if edge.uci == best_uci:
                score_w *= 1.15
            raw_weights.append((idx, max(1.0, min(float(MAX_WEIGHT), score_w))))

        # Guarantee the SF-top move wins strict BookRandom=false weight comparison.
        top_other = max((w for idx, w in raw_weights[1:]), default=0.0)
        for idx, w in raw_weights:
            edge, sc = passing[idx]
            int_w = int(round(w))
            if idx == 0 and top_other > 0:
                int_w = max(int_w, min(MAX_WEIGHT, int(round(top_other)) + 1))
            int_w = max(1, min(MAX_WEIGHT, int_w))
            weighted.append((edge, int_w, sc))
        selected[u] = weighted

    def _reachable_entries(cur_selected: dict[int, list[tuple[EdgeStats, int, int]]]):
        reach: set[int] = {dag.root_key}
        q: deque[int] = deque([dag.root_key])
        node_depth: dict[int, int] = {dag.root_key: 0}
        while q:
            u = q.popleft()
            d = node_depth[u]
            for edge, _, _ in cur_selected.get(u, []):
                v = edge.child_key
                if v not in reach:
                    reach.add(v)
                    node_depth[v] = d + 1
                    q.append(v)
        recs: list[tuple[int, int, int, int, int, int]] = []
        for u in reach:
            for edge, w, sc in cur_selected.get(u, []):
                recs.append((u, edge.encoded_move, w, sc, node_depth[u], edge.count))
        return recs

    recs = _reachable_entries(selected)
    max_records = max_size_bytes // polyglot.RECORD_STRUCT.size
    if len(recs) > max_records:
        # Iteratively prune lowest-priority deepest leaves first until within byte budget.
        while len(recs) > max_records:
            leaf_candidates = sorted(
                ((u, edge_w[1], edge_w[0].count, edge_w[0].uci)
                 for u, elist in selected.items()
                 for edge_w in elist
                 if edge_w[0].child_key not in selected),
                key=lambda item: (-dag.min_ply.get(item[0], 0), item[1], item[2], item[3]),
            )
            if not leaf_candidates:
                break
            drop_target = max(1, min(len(leaf_candidates), len(recs) - max_records))
            drops_by_node: dict[int, set[str]] = {}
            for u, _, _, uci in leaf_candidates[:drop_target]:
                drops_by_node.setdefault(u, set()).add(uci)
            for u, drop_ucis in drops_by_node.items():
                remaining = [item for item in selected[u] if item[0].uci not in drop_ucis]
                if remaining:
                    selected[u] = remaining
                else:
                    del selected[u]
            recs = _reachable_entries(selected)

    return [(u, enc_mv, w, 0) for u, enc_mv, w, _, _, _ in recs]


def build_verified_book(args: argparse.Namespace) -> bytes:
    """Run full pipeline and write output Polyglot binary book."""
    dag = extract_candidate_tree(
        sources=args.source,
        seed_openings=args.seed_openings,
        min_elo=args.min_elo,
        min_opponent=args.min_opponent,
        min_base_seconds=args.min_base_seconds,
        max_ply=args.max_ply,
        min_count=args.min_count,
        max_nodes=args.max_nodes,
        max_games=args.max_games,
    )
    evals = evaluate_and_expand_with_engine(
        dag=dag,
        engine_path=str(args.engine),
        workers=args.workers,
        nodes_limit=args.nodes,
        depth_limit=args.depth,
        hash_mb=args.hash_mb,
        expand_ply=args.expand_ply,
        max_nodes=args.max_nodes,
        cache_path=args.cache_jsonl,
    )
    records = minimax_and_prune(
        dag=dag,
        evals=evals,
        tolerance_cp=args.tolerance_cp,
        min_abs_cp=args.min_abs_cp,
        max_size_bytes=args.max_size_bytes,
    )
    payload = polyglot.write_book(args.output, records)
    unique_keys = {r[0] for r in records}
    sha = hashlib.sha256(payload).hexdigest()
    print(
        f"wrote {args.output}: positions={len(unique_keys)} entries={len(records)} "
        f"bytes={len(payload)} sha256={sha}",
        flush=True,
    )
    return payload


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", action="append", default=[], help="PGN or .pgn.zst globs")
    parser.add_argument("--seed-openings", action="append", default=[], help="Opening TSV/UCI files")
    parser.add_argument("--engine", type=pathlib.Path, required=True, help="UCI engine (Stockfish)")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--cache-jsonl", type=pathlib.Path, default=None)
    parser.add_argument("--min-elo", type=float, default=2050.0)
    parser.add_argument("--min-opponent", type=int, default=1900)
    parser.add_argument("--min-base-seconds", type=int, default=180)
    parser.add_argument("--max-ply", type=int, default=16)
    parser.add_argument("--min-count", type=int, default=4)
    parser.add_argument("--max-nodes", type=int, default=45000)
    parser.add_argument("--max-games", type=int, default=0)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--nodes", type=int, default=50000)
    parser.add_argument("--depth", type=int, default=0)
    parser.add_argument("--hash-mb", type=int, default=16)
    parser.add_argument("--expand-ply", type=int, default=8)
    parser.add_argument("--tolerance-cp", type=int, default=25)
    parser.add_argument("--min-abs-cp", type=int, default=-65)
    parser.add_argument("--max-size-bytes", type=int, default=2_000_000)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if not args.source and not args.seed_openings:
        print("build_verified_book.py: pass --source and/or --seed-openings", file=sys.stderr)
        return 2
    build_verified_book(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
