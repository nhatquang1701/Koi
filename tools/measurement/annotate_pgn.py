#!/usr/bin/env python3
"""Annotate PGN games using Koi's native UCI `annotate` move-quality analyzer."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

try:
    from tools.measurement import elo_oracle
except ImportError:  # pragma: no cover - direct CLI invocation
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import elo_oracle  # type: ignore[no-redef]


ANNOTATE_MOVE_PREFIX = "info string annotate move "
ANNOTATE_SUMMARY_PREFIX = "info string annotate summary "
ANNOTATE_ERROR_PREFIX = "info string annotate error:"
ANNOTATION_CATEGORIES = (
    "brilliant",
    "great",
    "best",
    "excellent",
    "good",
    "book",
    "forced",
    "inaccuracy",
    "mistake",
    "miss",
    "blunder",
)


def parse_annotate_move_line(line: str) -> Optional[Dict[str, Any]]:
    """Parse one `info string annotate move ...` protocol line emitted by Koi."""

    stripped = line.strip()
    if not stripped.startswith(ANNOTATE_MOVE_PREFIX):
        return None
    tokens = stripped[len(ANNOTATE_MOVE_PREFIX) :].split()
    if not tokens:
        return None

    record: Dict[str, Any] = {
        "move": tokens[0],
        "ply": 0,
        "side": "white",
        "best": tokens[0],
        "classification": "good",
        "cpl": 0,
        "accuracy": 100.0,
        "wp_before": 0.5,
        "wp_after": 0.5,
        "eval_cp": 0,
        "eval_mate": None,
        "best_eval_cp": 0,
        "best_eval_mate": None,
        "depth": 0,
        "glyph": "",
        "nag": None,
        "pv": [],
    }
    index = 1
    while index < len(tokens):
        key = tokens[index]
        if key == "pv":
            record["pv"] = tokens[index + 1 :]
            break
        if key in {"eval", "best_eval"}:
            if index + 2 >= len(tokens):
                break
            score_type = tokens[index + 1]
            score_val = int(tokens[index + 2])
            if key == "eval":
                if score_type == "mate":
                    record["eval_mate"] = score_val
                    record["eval_cp"] = (
                        elo_oracle.MATE_SCORE_CP if score_val > 0 else -elo_oracle.MATE_SCORE_CP
                    )
                else:
                    record["eval_cp"] = score_val
                    record["eval_mate"] = None
            else:
                if score_type == "mate":
                    record["best_eval_mate"] = score_val
                    record["best_eval_cp"] = (
                        elo_oracle.MATE_SCORE_CP if score_val > 0 else -elo_oracle.MATE_SCORE_CP
                    )
                else:
                    record["best_eval_cp"] = score_val
                    record["best_eval_mate"] = None
            index += 3
            continue

        if index + 1 >= len(tokens):
            break
        val = tokens[index + 1]
        if key in {"ply", "cpl", "depth", "nag"}:
            record[key] = int(val)
        elif key in {"accuracy", "wp_before", "wp_after"}:
            record[key] = float(val)
        elif key in {"side", "best", "classification", "glyph"}:
            record[key] = val
        index += 2

    return record


def parse_annotate_summary_line(line: str) -> Optional[Dict[str, Any]]:
    """Parse one `info string annotate summary <side> ...` line emitted by Koi."""

    stripped = line.strip()
    if not stripped.startswith(ANNOTATE_SUMMARY_PREFIX):
        return None
    tokens = stripped[len(ANNOTATE_SUMMARY_PREFIX) :].split()
    if not tokens:
        return None

    summary: Dict[str, Any] = {
        "side": tokens[0],
        "moves": 0,
        "accuracy": 100.0,
        "avg_cpl": 0.0,
    }
    for cat in ANNOTATION_CATEGORIES:
        summary[cat] = 0

    index = 1
    while index + 1 < len(tokens):
        key = tokens[index]
        val = tokens[index + 1]
        if key in {"accuracy", "avg_cpl"}:
            summary[key] = float(val)
        elif key == "moves" or key in ANNOTATION_CATEGORIES:
            summary[key] = int(val)
        index += 2
    return summary


def empty_summary(side: str) -> Dict[str, Any]:
    summary: Dict[str, Any] = {
        "side": side,
        "moves": 0,
        "accuracy": 100.0,
        "avg_cpl": 0.0,
    }
    for cat in ANNOTATION_CATEGORIES:
        summary[cat] = 0
    return summary


def format_pgn_eval_tag(score_cp: int, mate: Optional[int], side: str) -> str:
    """Format a PGN `[%eval ...]` token from White's perspective."""

    if mate is not None:
        white_mate = mate if side == "white" else -mate
        return f"#{white_mate}"
    white_cp = score_cp if side == "white" else -score_cp
    return f"{white_cp / 100.0:+.2f}"


def _annotate_game_with_engine(
    engine: elo_oracle.UciEngine,
    start_fen: Optional[str],
    uci_moves: Sequence[str],
    *,
    depth: Optional[int],
    movetime_ms: Optional[int],
    nodes: Optional[int],
    engine_options: Dict[str, Any],
) -> Tuple[List[Dict[str, Any]], Dict[str, Dict[str, Any]]]:
    if not uci_moves:
        return [], {"white": empty_summary("white"), "black": empty_summary("black")}

    engine._set_options(engine_options)
    engine._wait_ready()

    position_cmd = f"position fen {start_fen}" if start_fen else "position startpos"
    position_cmd += " moves " + " ".join(uci_moves)
    engine._send(position_cmd)

    limit_parts: List[str] = []
    if movetime_ms is not None:
        limit_parts.extend(["movetime", str(movetime_ms)])
    if depth is not None:
        limit_parts.extend(["depth", str(depth)])
    if nodes is not None:
        limit_parts.extend(["nodes", str(nodes)])
    if not limit_parts:
        limit_parts.extend(["depth", "4"])

    engine._send("annotate game " + " ".join(limit_parts))
    engine._send("isready")

    move_records: List[Dict[str, Any]] = []
    summaries: Dict[str, Dict[str, Any]] = {
        "white": empty_summary("white"),
        "black": empty_summary("black"),
    }
    while True:
        line = engine._read_line("annotate game")
        if line == "readyok":
            break
        if line.startswith(ANNOTATE_ERROR_PREFIX):
            raise elo_oracle.OracleError(line)
        parsed_move = parse_annotate_move_line(line)
        if parsed_move is not None:
            move_records.append(parsed_move)
            continue
        parsed_summary = parse_annotate_summary_line(line)
        if parsed_summary is not None:
            summaries[parsed_summary["side"]] = parsed_summary
    return move_records, summaries


def _wrap_pgn_tokens(tokens: Sequence[str], width: int = 80) -> str:
    lines: List[str] = []
    current = ""
    for token in tokens:
        if not current:
            current = token
        elif len(current) + 1 + len(token) <= width:
            current += " " + token
        else:
            lines.append(current)
            current = token
    if current:
        lines.append(current)
    return "\n".join(lines)


def _format_annotated_pgn(
    chess: Any,
    game: Any,
    move_records: Sequence[Dict[str, Any]],
    summaries: Dict[str, Dict[str, Any]],
    *,
    include_glyphs: bool,
) -> Tuple[str, List[Dict[str, Any]]]:
    headers = dict(game.headers)
    headers["Annotator"] = "Koi"
    headers["WhiteAccuracy"] = f"{summaries['white']['accuracy']:.1f}%"
    headers["BlackAccuracy"] = f"{summaries['black']['accuracy']:.1f}%"

    board = game.board()
    tokens: List[str] = []
    json_moves: List[Dict[str, Any]] = []
    force_move_number = True

    mainline_nodes = list(game.mainline())
    if len(move_records) != len(mainline_nodes):
        raise elo_oracle.OracleError(
            f"Koi emitted {len(move_records)} annotated moves for a {len(mainline_nodes)}-ply game."
        )

    for node, rec in zip(mainline_nodes, move_records):
        move_obj = node.move
        san = board.san(move_obj)
        side = "white" if board.turn == chess.WHITE else "black"
        move_number = board.fullmove_number
        if side == "white":
            tokens.append(f"{move_number}.")
        elif force_move_number:
            tokens.append(f"{move_number}...")

        eval_tag = format_pgn_eval_tag(rec["eval_cp"], rec["eval_mate"], rec["side"])
        comment_parts = [
            f"[{rec['classification']}]",
            f"[%eval {eval_tag}]",
            f"cpl={rec['cpl']}",
            f"acc={rec['accuracy']:.1f}%",
        ]
        best_san = san
        if rec["best"] != rec["move"]:
            try:
                best_obj = chess.Move.from_uci(rec["best"])
                if board.is_legal(best_obj):
                    best_san = board.san(best_obj)
                else:
                    best_san = rec["best"]
            except ValueError:
                best_san = rec["best"]
            comment_parts.append(f"best={best_san}")

        annotated_move = elo_oracle.format_pgn_move_annotation(
            san,
            rec["classification"],
            comment=" ".join(comment_parts),
            include_nag=True,
            include_glyph=include_glyphs,
        )
        tokens.append(annotated_move)
        force_move_number = True  # Every annotated move carries a {comment}.

        enriched = dict(rec)
        enriched["san"] = san
        enriched["best_san"] = best_san
        enriched["pgn_eval"] = eval_tag
        json_moves.append(enriched)
        board.push(move_obj)

    w_sum = summaries["white"]
    b_sum = summaries["black"]
    summary_comment = (
        f"{{Koi summary: White {w_sum['accuracy']:.1f}% (avg cpl {w_sum['avg_cpl']:.1f}, "
        f"blunders={w_sum['blunder']}, mistakes={w_sum['mistake']}, inaccuracies={w_sum['inaccuracy']}); "
        f"Black {b_sum['accuracy']:.1f}% (avg cpl {b_sum['avg_cpl']:.1f}, "
        f"blunders={b_sum['blunder']}, mistakes={b_sum['mistake']}, inaccuracies={b_sum['inaccuracy']})}}"
    )
    tokens.append(summary_comment)
    tokens.append(headers.get("Result", "*"))

    header_lines = [
        f'[{key} "{str(value).replace(chr(34), chr(39))}"]'
        for key, value in headers.items()
    ]
    pgn_text = "\n".join(header_lines) + "\n\n" + _wrap_pgn_tokens(tokens) + "\n"
    return pgn_text, json_moves


def annotate_pgn_file(
    pgn_path: Path,
    koi_path: Path,
    *,
    depth: Optional[int] = None,
    movetime_ms: Optional[int] = None,
    nodes: Optional[int] = None,
    threads: int = 1,
    hash_mb: Optional[int] = None,
    own_book: bool = False,
    max_games: Optional[int] = None,
    include_glyphs: bool = False,
) -> Dict[str, Any]:
    """Read `pgn_path`, annotate every game with Koi, and return PGN text + structured JSON."""

    chess, pgn = elo_oracle._load_chess_modules()
    raw_pgn, pgn_bytes = elo_oracle._read_pgn(pgn_path)
    stream = io.StringIO(raw_pgn)

    games: List[Any] = []
    while True:
        if max_games is not None and len(games) >= max_games:
            break
        game = pgn.read_game(stream)
        if game is None:
            break
        if game.errors:
            raise elo_oracle.OracleError(f"Invalid PGN in '{pgn_path}': {game.errors[0]}")
        games.append(game)

    if not games:
        raise elo_oracle.OracleError(f"No PGN games found in '{pgn_path}'.")

    engine_options: Dict[str, Any] = {
        "OwnBook": bool(own_book),
        "Threads": max(1, int(threads)),
    }
    if hash_mb is not None:
        engine_options["Hash"] = max(1, int(hash_mb))

    annotated_pgn_blocks: List[str] = []
    report_games: List[Dict[str, Any]] = []

    with elo_oracle.UciEngine(koi_path, "Koi") as koi:
        engine_metadata = koi.metadata(engine_options)
        for index, game in enumerate(games, start=1):
            start_fen = game.headers.get("FEN")
            uci_moves = [node.move.uci() for node in game.mainline() if node.move is not None]
            move_records, summaries = _annotate_game_with_engine(
                koi,
                start_fen,
                uci_moves,
                depth=depth,
                movetime_ms=movetime_ms,
                nodes=nodes,
                engine_options=engine_options,
            )
            block_text, json_moves = _format_annotated_pgn(
                chess,
                game,
                move_records,
                summaries,
                include_glyphs=include_glyphs,
            )
            annotated_pgn_blocks.append(block_text)
            report_games.append(
                {
                    "game_index": index,
                    "headers": dict(game.headers),
                    "white_summary": summaries["white"],
                    "black_summary": summaries["black"],
                    "moves": json_moves,
                }
            )

    return {
        "schema": "koi-pgn-annotation-v1",
        "source_pgn": str(pgn_path),
        "source_sha256": hashlib.sha256(pgn_bytes).hexdigest(),
        "engine": engine_metadata,
        "limits": {
            "depth": depth if (depth is not None or (movetime_ms is None and nodes is None)) else None,
            "movetime_ms": movetime_ms,
            "nodes": nodes,
        },
        "game_count": len(report_games),
        "games": report_games,
        "annotated_pgn": "\n".join(annotated_pgn_blocks),
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Annotate PGN games with Koi move classifications, NAGs, [%eval], and accuracy."
    )
    parser.add_argument("--pgn", type=Path, required=True, help="Input PGN file.")
    parser.add_argument("--koi", type=Path, required=True, help="Path to Koi engine binary.")
    parser.add_argument("--depth", type=int, default=None, help="Search depth per move (default: 4 if no other limit).")
    parser.add_argument("--movetime-ms", type=int, default=None, help="Fixed time budget per move in ms.")
    parser.add_argument("--nodes", type=int, default=None, help="Fixed node budget per move.")
    parser.add_argument("--threads", type=int, default=1, help="UCI Threads option (default: 1).")
    parser.add_argument("--hash-mb", type=int, default=None, help="Optional UCI Hash size in MiB.")
    parser.add_argument("--own-book", action="store_true", help="Enable Koi OwnBook during annotation.")
    parser.add_argument("--max-games", type=int, default=None, help="Maximum games to annotate.")
    parser.add_argument(
        "--include-glyphs",
        action="store_true",
        help="Include suffix glyphs (!, ?!, ?, ??) on SAN in addition to $N NAGs.",
    )
    parser.add_argument("--output-pgn", type=Path, default=None, help="Write annotated PGN output here.")
    parser.add_argument("--output-json", type=Path, default=None, help="Write JSON report here.")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        koi_path = elo_oracle._resolve_file(args.koi, "Koi executable", "--koi <koi-engine>")
        pgn_path = elo_oracle._resolve_file(args.pgn, "PGN file", "--pgn <file.pgn>")
        result = annotate_pgn_file(
            pgn_path,
            koi_path,
            depth=args.depth,
            movetime_ms=args.movetime_ms,
            nodes=args.nodes,
            threads=args.threads,
            hash_mb=args.hash_mb,
            own_book=args.own_book,
            max_games=args.max_games,
            include_glyphs=args.include_glyphs,
        )
        if args.output_pgn is not None:
            args.output_pgn.parent.mkdir(parents=True, exist_ok=True)
            args.output_pgn.write_text(result["annotated_pgn"], encoding="utf-8")
        if args.output_json is not None:
            payload = {k: v for k, v in result.items() if k != "annotated_pgn"}
            args.output_json.parent.mkdir(parents=True, exist_ok=True)
            args.output_json.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        if args.output_pgn is None and args.output_json is None:
            sys.stdout.write(result["annotated_pgn"])
    except elo_oracle.OracleError as error:
        print(f"Error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
