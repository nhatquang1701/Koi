import pathlib

p = pathlib.Path("artifacts/plans/ccrl-eligibility-2026-11-01/crash_1000.py")
t = p.read_text(encoding="utf-8")

start = t.index("def configure_engine(")
end = t.index("def play_game(")
new_configure = '''def configure_engine(engine: Engine, is_koi: bool) -> None:
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


'''
new_play = '''def play_game(config: dict) -> dict:
    game_id = config["game_id"]
    opening_name, opening_moves = config["openings"][game_id % len(config["openings"])]
    board = chess.Board()
    for move in opening_moves:
        board.push(move)
    koi = Engine(config["engine"])
    if config.get("opponent"):
        opponent = Engine(config["opponent"])
        opponent_is_koi = False
    else:
        # Self-play still runs two independent processes, but both are Koi so
        # every observed error counts toward eligibility.
        opponent = Engine(config["engine"])
        opponent_is_koi = True
    koi_white = bool(config.get("koi_white", True))
    engines = [koi, opponent] if koi_white else [opponent, koi]
    is_koi = [(engine is koi) or opponent_is_koi for engine in engines]
    koi_errors: list[str] = []
    opponent_errors: list[str] = []
    started = time.time()
    plies = 0
    try:
        for index, engine in enumerate(engines):
            configure_engine(engine, is_koi[index])
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
    except Exception as exc:  # noqa: BLE001 - setup failure, attribute to Koi
        outcome = "error"
        result = "*"
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


'''
worker_start = t.index("def _worker(")
t = t[:start] + new_configure + new_play + t[worker_start:]

# main(): add opponent option, alternate colours, separate Koi/opponent sums.
t = t.replace(
    '''    parser.add_argument(
        "--openings",
        default="tests/data/openings/openings-curated-32.txt",
    )''',
    '''    parser.add_argument(
        "--openings",
        default="tests/data/openings/openings-curated-32.txt",
    )
    parser.add_argument(
        "--opponent",
        default="",
        help="Optional second engine (e.g. Stockfish 19) to diversify games; "
        "empty means Koi self-play.",
    )''',
    1,
)
t = t.replace(
    '''    engine = str(Path(args.engine).resolve())
    if not Path(engine).is_file():
        raise SystemExit(f"engine not found: {engine}")''',
    '''    engine = str(Path(args.engine).resolve())
    if not Path(engine).is_file():
        raise SystemExit(f"engine not found: {engine}")
    opponent = str(Path(args.opponent).resolve()) if args.opponent else ""
    if opponent and not Path(opponent).is_file():
        raise SystemExit(f"opponent not found: {opponent}")''',
    1,
)
t = t.replace(
    '''            "move_timeout_s": args.move_timeout_s,
            "seed": args.seed,
        }''',
    '''            "move_timeout_s": args.move_timeout_s,
            "seed": args.seed,
            "opponent": opponent,
            "koi_white": (game_id % 2 == 0) if opponent else True,
        }''',
    1,
)
t = t.replace(
    '''        "games_with_errors": failures,
        "crashes": sum(
            1 for r in results if any("non-zero engine exit" in e or "stderr" in e for e in r["errors"])
        ),
        "timeouts": sum(1 for r in results if any("Timeout" in e for e in r["errors"])),
        "illegal_moves": sum(1 for r in results if any("illegal bestmove" in e for e in r["errors"])),
        "protocol_violations": sum(
            1 for r in results if any("protocol violation" in e or "non-UCI" in e for e in r["errors"])
        ),''',
    '''        "games_with_errors": failures,
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
        ][:3],''',
    1,
)
t = t.replace(
    '''            if result["errors"]:
                    failures += 1''',
    '''            if result["errors"]:
                    failures += 1''',
    1,
)
p.write_text(t, encoding="utf-8", newline="\n")
print("harness now supports a second engine opponent")
