import hashlib
import json
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

import tools.measurement.annotate_pgn as annotate_pgn
import tools.measurement.elo_oracle as elo_oracle


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY_ROOT / "tests" / "support"))
import unittest_progress  # noqa: E402,F401
ORACLE_SCRIPT = REPOSITORY_ROOT / "tools" / "measurement" / "elo_oracle.py"
MATCH_SCRIPT = REPOSITORY_ROOT / "tools" / "measurement" / "stockfish_match.py"
BOOK_AUDIT_SCRIPT = REPOSITORY_ROOT / "tools" / "measurement" / "book_audit.py"


FAKE_ENGINE_SOURCE = textwrap.dedent(
    """
    import sys

    role = sys.argv[1]
    log_path = sys.argv[2]
    own_book = False
    side_to_move = "w"

    def emit(line):
        print(line, flush=True)

    for raw_line in sys.stdin:
        line = raw_line.strip()
        with open(log_path, "a", encoding="utf-8") as log:
            log.write(line + "\\n")
        if line == "uci":
            emit("id name Fake Koi 1.1.0" if role == "koi" else "id name Fake Stockfish 17.1")
            emit("id author Task 1 test")
            emit("option name Threads type spin default 1 min 1 max 64")
            if role == "koi":
                emit("option name OwnBook type check default false")
                emit("option name BookFile type string default book.bin")
                emit("option name BookDepth type spin default 16 min 0 max 40")
                emit("option name BookRandom type check default false")
                emit("option name Speed type spin default 100 min 1 max 100")
            emit("uciok")
        elif line == "isready":
            emit("readyok")
        elif line.startswith("setoption name OwnBook value "):
            own_book = line.endswith("true")
        elif line.startswith("position fen "):
            side_to_move = line.split()[3]
        elif line.startswith("go "):
            if role == "koi" and own_book:
                emit("info string book move e2e4 depth 7")
            if role == "stockfish":
                score = 100 if side_to_move == "w" else -100
                emit(f"info depth 1 score cp {score} nodes 10 nps 20 time 1 pv e2e4")
            emit("bestmove e2e4")
        elif line == "quit":
            break
    """
)


def write_fake_engine(temporary_path: Path, role: str) -> tuple[Path, Path]:
    script_path = temporary_path / f"fake {role} engine.py"
    log_path = temporary_path / f"{role} commands.log"
    if os.name == "nt":
        script_path.write_text(FAKE_ENGINE_SOURCE, encoding="utf-8")
        command_path = temporary_path / f"fake {role} engine.cmd"
        command_path.write_text(
            f'@"{sys.executable}" "{script_path}" {role} "{log_path}"\n',
            encoding="utf-8",
        )
    else:
        # POSIX executes the command directly, so a shell wrapper mirrors the
        # Windows .cmd shim and passes the role and log path as arguments.
        script_path.write_text(FAKE_ENGINE_SOURCE, encoding="utf-8")
        command_path = temporary_path / f"fake {role} engine.sh"
        command_path.write_text(
            "#!/bin/sh\n" f'exec "{sys.executable}" "{script_path}" {role} "{log_path}"\n',
            encoding="utf-8",
        )
        command_path.chmod(0o755)
    return command_path, log_path


class EloOracleExtractionTests(unittest.TestCase):
    def test_parse_stockfish_terminal_none_bestmove_as_no_move(self):
        self.assertEqual(
            elo_oracle.parse_bestmove_line("bestmove (none)"),
            ("0000", None),
        )

    def test_normalize_score_uses_white_perspective_and_fixed_mate_scale(self):
        self.assertEqual(elo_oracle.normalize_score("cp", 37, "white"), 37)
        self.assertEqual(elo_oracle.normalize_score("cp", 37, "black"), -37)
        self.assertEqual(elo_oracle.normalize_score("mate", 4, "white"), 100000)
        self.assertEqual(elo_oracle.normalize_score("mate", -2, "black"), 100000)

    def test_cpl_is_loss_from_the_moving_side(self):
        self.assertEqual(elo_oracle.calculate_cpl(120, 80, "white"), 40)
        self.assertEqual(elo_oracle.calculate_cpl(-120, -80, "black"), 40)
        self.assertEqual(elo_oracle.calculate_cpl(80, 120, "white"), 0)

    def test_analysis_requires_external_engine_paths(self):
        with tempfile.TemporaryDirectory(prefix="koi oracle ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_path = temporary_path / "fixture.pgn"
            report_path = temporary_path / "report.json"
            pgn_path.write_text('[Result "*"]\n\n1. e4 *\n', encoding="utf-8")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(ORACLE_SCRIPT),
                    "--pgn",
                    str(pgn_path),
                    "--output",
                    str(report_path),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("--koi", completed.stderr)
        self.assertIn("--stockfish", completed.stderr)

    def test_cli_rejects_threads_other_than_the_approved_four(self):
        with tempfile.TemporaryDirectory(prefix="koi oracle ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_path = temporary_path / "fixture.pgn"
            report_path = temporary_path / "report.json"
            pgn_path.write_text('[Result "*"]\n\n1. e4 *\n', encoding="utf-8")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(ORACLE_SCRIPT),
                    "--pgn",
                    str(pgn_path),
                    "--output",
                    str(report_path),
                    "--extract-only",
                    "--threads",
                    "1",
                ],
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("exactly 4", completed.stderr)

    def test_analysis_records_separate_cpl_values_and_engine_options(self):
        with tempfile.TemporaryDirectory(prefix="koi oracle ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_path = temporary_path / "fixture game.pgn"
            report_path = temporary_path / "analysis report.json"
            pgn_path.write_text('[Result "*"]\n\n1. e4 *\n', encoding="utf-8")
            koi_path, koi_log_path = write_fake_engine(temporary_path, "koi")
            stockfish_path, _stockfish_log_path = write_fake_engine(temporary_path, "stockfish")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(ORACLE_SCRIPT),
                    "--pgn",
                    str(pgn_path),
                    "--koi",
                    str(koi_path),
                    "--stockfish",
                    str(stockfish_path),
                    "--output",
                    str(report_path),
                    "--movetime-ms",
                    "1",
                    "--threads",
                    "4",
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(report_path.read_text(encoding="utf-8"))
            koi_commands = koi_log_path.read_text(encoding="utf-8")
            expected_koi_sha256 = hashlib.sha256(koi_path.read_bytes()).hexdigest()

        position = report["games"][0]["positions"][0]
        self.assertEqual(report["mode"], "analysis")
        self.assertFalse(report["search_metrics"]["includes_book_hits"])
        self.assertEqual(report["search_metrics"]["positions"], 1)
        self.assertEqual(position["search"]["actual_game_move"]["cpl"], 0)
        self.assertEqual(position["search"]["koi_suggested_move"]["cpl"], 0)
        self.assertEqual(position["search"]["koi_suggested_move"]["uci"], "e2e4")
        self.assertEqual(position["search"]["timings_ms"]["koi_search"] >= 0, True)
        self.assertEqual(report["engines"]["koi"]["identity"]["name"], "Fake Koi 1.1.0")
        self.assertEqual(report["engines"]["koi"]["version"], "1.1.0")
        self.assertEqual(
            report["engines"]["koi"]["hashes"]["executable_sha256"],
            expected_koi_sha256,
        )
        self.assertEqual(report["engines"]["koi"]["options"]["OwnBook"], False)
        self.assertEqual(report["engines"]["koi"]["options"]["Threads"], 4)
        self.assertEqual(report["engines"]["koi"]["options"]["Speed"], 100)
        self.assertIn("setoption name OwnBook value false", koi_commands)
        self.assertIn("setoption name Threads value 4", koi_commands)
        self.assertIn("setoption name Speed value 100", koi_commands)
        self.assertIn("go movetime 1", koi_commands)
        self.assertEqual(position["search"]["actual_game_move"]["classification"], "best")
        self.assertEqual(position["search"]["koi_suggested_move"]["classification"], "best")
        self.assertEqual(report["measurement"]["network"]["state"], "disabled")
        self.assertEqual(report["measurement"]["book"]["state"], "disabled")
        self.assertEqual(report["measurement"]["tablebase"]["state"], "disabled")
        self.assertGreaterEqual(report["hardware"]["cpu_count"], 1)
        self.assertIn("executable_sha256", report["engines"]["stockfish"]["hashes"])

    def test_extract_preserves_annotations_on_mainline_positions(self):
        pgn = '[Result "*"]\n\n1. e4 $1 {[%eval +0.25] [%clk 0:05:00] coach note} e5 *\n'

        games = elo_oracle.extract_games(pgn)

        annotations = games[0]["positions"][0]["annotations"]
        self.assertEqual(annotations["comment"], "[%eval +0.25] [%clk 0:05:00] coach note")
        self.assertEqual(annotations["nags"], [1])
        self.assertEqual(annotations["eval"], "+0.25")
        self.assertEqual(annotations["clock"], "0:05:00")

    def test_classify_cpl_uses_stable_forensic_bands(self):
        self.assertEqual(elo_oracle.classify_cpl(0), "best")
        self.assertEqual(elo_oracle.classify_cpl(49), "good")
        self.assertEqual(elo_oracle.classify_cpl(50), "inaccuracy")
        self.assertEqual(elo_oracle.classify_cpl(99), "inaccuracy")
        self.assertEqual(elo_oracle.classify_cpl(100), "mistake")
        self.assertEqual(elo_oracle.classify_cpl(299), "mistake")
        self.assertEqual(elo_oracle.classify_cpl(300), "blunder")
        self.assertIsNone(elo_oracle.classify_cpl(None))

    def test_book_audit_keeps_book_hits_and_cpl_outside_search_metrics(self):
        with tempfile.TemporaryDirectory(prefix="koi oracle ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_path = temporary_path / "fixture game.pgn"
            book_path = temporary_path / "licensed book.bin"
            report_path = temporary_path / "book audit report.json"
            pgn_path.write_text('[Result "*"]\n\n1. e4 *\n', encoding="utf-8")
            book_path.write_bytes(b"synthetic test book")
            koi_path, koi_log_path = write_fake_engine(temporary_path, "koi")
            stockfish_path, _stockfish_log_path = write_fake_engine(temporary_path, "stockfish")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(ORACLE_SCRIPT),
                    "--pgn",
                    str(pgn_path),
                    "--koi",
                    str(koi_path),
                    "--stockfish",
                    str(stockfish_path),
                    "--output",
                    str(report_path),
                    "--movetime-ms",
                    "1",
                    "--threads",
                    "4",
                    "--book",
                    str(book_path),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(report_path.read_text(encoding="utf-8"))
            koi_commands = koi_log_path.read_text(encoding="utf-8")

        position = report["games"][0]["positions"][0]["book_audit"]
        self.assertEqual(report["mode"], "book-audit")
        self.assertIsNone(report["search_metrics"])
        self.assertEqual(report["book_audit"]["book_hits"], 1)
        self.assertEqual(report["book_audit"]["settings"]["BookDepth"], 16)
        self.assertFalse(report["book_audit"]["settings"]["BookRandom"])
        self.assertTrue(position["book_used"])
        self.assertEqual(position["book_move"], "e2e4")
        self.assertEqual(position["book_ply"], 7)
        self.assertEqual(position["stockfish_cpl"], 0)
        self.assertEqual(position["actual_game_move_stockfish_cpl"], 0)
        self.assertIn("setoption name OwnBook value true", koi_commands)
        self.assertIn("setoption name BookDepth value 16", koi_commands)
        self.assertIn("setoption name BookRandom value false", koi_commands)
        self.assertIn(f"setoption name BookFile value {Path(book_path).resolve()}", koi_commands)

    def test_extract_only_records_mainline_san_positions_and_metadata(self):
        pgn = """[Event \"Extraction\"]
[Site \"Test\"]
[Date \"2026.09.04\"]
[Round \"1\"]
[White \"White\"]
[Black \"Black\"]
[Result \"*\"]

1. e4 {king pawn} (1. d4 d5) e5 2. Nf3 Nc6 3. Bc4 Nf6 4. O-O Be7 *

[Event \"Promotion\"]
[SetUp \"1\"]
[FEN \"7k/P7/8/8/8/8/8/6K1 w - - 0 1\"]
[Result \"*\"]

1. a8=Q+ *

[Event \"Black FEN\"]
[SetUp \"1\"]
[FEN \"7k/8/8/8/8/8/8/7K b - - 7 23\"]
[Result \"*\"]

23... Kg8 *
"""

        with tempfile.TemporaryDirectory(prefix="koi oracle ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_path = temporary_path / "fixture game.pgn"
            report_path = temporary_path / "extraction report.json"
            pgn_path.write_text(pgn, encoding="utf-8")
            expected_pgn_sha256 = hashlib.sha256(pgn_path.read_bytes()).hexdigest()

            completed = subprocess.run(
                [
                    sys.executable,
                    str(ORACLE_SCRIPT),
                    "--pgn",
                    str(pgn_path),
                    "--output",
                    str(report_path),
                    "--extract-only",
                    "--koi",
                    str(temporary_path / "missing koi.exe"),
                    "--stockfish",
                    str(temporary_path / "missing stockfish.exe"),
                    "--book",
                    str(temporary_path / "missing book.bin"),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(report_path.read_text(encoding="utf-8"))

        self.assertEqual(report["schema"], "koi-elo-oracle")
        self.assertEqual(report["schema_version"], 1)
        self.assertEqual(report["mode"], "extract-only")
        self.assertEqual(
            report["source"]["pgn_sha256"],
            expected_pgn_sha256,
        )
        self.assertRegex(report["created_at"], r"^20\d\d-\d\d-\d\dT")

        self.assertEqual(len(report["games"]), 3)
        first_game = report["games"][0]
        self.assertEqual(len(first_game["positions"]), 8)
        self.assertEqual(first_game["positions"][0]["fen"], "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1")
        self.assertEqual(first_game["positions"][0]["ply"], 1)
        self.assertEqual(first_game["positions"][0]["side"], "white")
        self.assertEqual(first_game["positions"][0]["actual_move_uci"], "e2e4")
        self.assertEqual(first_game["positions"][0]["actual_move_san"], "e4")

        castling_position = next(
            position
            for position in first_game["positions"]
            if position["actual_move_san"] == "O-O"
        )
        self.assertEqual(castling_position["ply"], 7)
        self.assertEqual(castling_position["actual_move_uci"], "e1g1")
        self.assertNotIn("d2d4", [position["actual_move_uci"] for position in first_game["positions"]])

        promotion_position = report["games"][1]["positions"][0]
        self.assertEqual(promotion_position["fen"], "7k/P7/8/8/8/8/8/6K1 w - - 0 1")
        self.assertEqual(promotion_position["side"], "white")
        self.assertEqual(promotion_position["actual_move_uci"], "a7a8q")
        self.assertEqual(promotion_position["actual_move_san"], "a8=Q+")

        black_fen_position = report["games"][2]["positions"][0]
        self.assertEqual(black_fen_position["ply"], 1)
        self.assertEqual(black_fen_position["move_number"], 23)
        self.assertEqual(black_fen_position["side"], "black")
        self.assertEqual(black_fen_position["fen"], "7k/8/8/8/8/8/8/7K b - - 7 23")
        self.assertEqual(black_fen_position["actual_move_uci"], "h8g8")
        self.assertEqual(black_fen_position["actual_move_san"], "Kg8")

    def test_match_entrypoint_preserves_zero_random_seed_and_shell_free_arguments(self):
        import tools.measurement.stockfish_match as stockfish_match

        args = stockfish_match._build_parser().parse_args(
            [
                "--koi",
                "C:\\Koi\\koi.exe",
                "--stockfish",
                "C:\\Engines\\stockfish.exe",
                "--random-seed",
                "0",
                "--movetime-ms",
                "25",
            ]
        )
        command = stockfish_match.build_command(args, powershell="pwsh-test")

        self.assertEqual(command[0], "pwsh-test")
        self.assertIn("-KoiRandomSeed", command)
        self.assertEqual(command[command.index("-KoiRandomSeed") + 1], "0")
        self.assertIn("-MovetimeMs", command)
        self.assertNotIn("-Depth", command)
        self.assertTrue(args.output_directory.resolve().is_relative_to((REPOSITORY_ROOT / "artifacts").resolve()))

    def test_extract_only_defaults_to_a_repository_artifact_and_announces_path(self):
        with tempfile.TemporaryDirectory(prefix="koi oracle ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_path = temporary_path / "fixture.pgn"
            pgn_path.write_text('[Result "*"]\n\n1. e4 *\n', encoding="utf-8")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(ORACLE_SCRIPT),
                    "--pgn",
                    str(pgn_path),
                    "--extract-only",
                ],
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stderr, "")
        report_lines = [line for line in completed.stdout.splitlines() if line.startswith("report ")]
        self.assertEqual(len(report_lines), 1, completed.stdout)
        report_path = Path(report_lines[0].split(" ", 1)[1]).resolve()
        self.assertTrue(report_path.is_relative_to((REPOSITORY_ROOT / "artifacts").resolve()))
        self.assertTrue(report_path.is_file())
        self.assertEqual(json.loads(report_path.read_text(encoding="utf-8"))["mode"], "extract-only")
        report_path.unlink(missing_ok=True)

    def test_named_tool_entrypoints_have_safe_help_without_starting_engines(self):
        for script, required_text in (
            (MATCH_SCRIPT, "--koi"),
            (BOOK_AUDIT_SCRIPT, "--book"),
        ):
            completed = subprocess.run(
                [sys.executable, str(script), "--help"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stderr, "")
            self.assertIn(required_text, completed.stdout)

    def test_book_audit_entrypoint_forwards_to_the_existing_report_schema(self):
        with tempfile.TemporaryDirectory(prefix="koi oracle ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_path = temporary_path / "fixture game.pgn"
            book_path = temporary_path / "licensed book.bin"
            report_path = temporary_path / "book audit report.json"
            pgn_path.write_text('[Result "*"]\n\n1. e4 *\n', encoding="utf-8")
            book_path.write_bytes(b"synthetic test book")
            koi_path, _koi_log_path = write_fake_engine(temporary_path, "koi")
            stockfish_path, _stockfish_log_path = write_fake_engine(temporary_path, "stockfish")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(BOOK_AUDIT_SCRIPT),
                    "--pgn",
                    str(pgn_path),
                    "--koi",
                    str(koi_path),
                    "--stockfish",
                    str(stockfish_path),
                    "--book",
                    str(book_path),
                    "--output",
                    str(report_path),
                    "--movetime-ms",
                    "1",
                    "--threads",
                    "4",
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(report_path.read_text(encoding="utf-8"))

        self.assertEqual(report["schema"], "koi-elo-oracle")
        self.assertEqual(report["mode"], "book-audit")

    def test_book_audit_entrypoint_defaults_to_a_repository_artifact(self):
        with tempfile.TemporaryDirectory(prefix="koi oracle ") as temporary_directory:
            temporary_path = Path(temporary_directory)
            pgn_path = temporary_path / "fixture.pgn"
            book_path = temporary_path / "licensed book.bin"
            pgn_path.write_text('[Result "*"]\n\n1. e4 *\n', encoding="utf-8")
            book_path.write_bytes(b"synthetic test book")
            koi_path, _koi_log_path = write_fake_engine(temporary_path, "koi")
            stockfish_path, _stockfish_log_path = write_fake_engine(temporary_path, "stockfish")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(BOOK_AUDIT_SCRIPT),
                    "--pgn",
                    str(pgn_path),
                    "--koi",
                    str(koi_path),
                    "--stockfish",
                    str(stockfish_path),
                    "--book",
                    str(book_path),
                    "--movetime-ms",
                    "1",
                    "--threads",
                    "4",
                ],
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stderr, "")
        report_lines = [line for line in completed.stdout.splitlines() if line.startswith("report ")]
        self.assertEqual(len(report_lines), 1, completed.stdout)
        report_path = Path(report_lines[0].split(" ", 1)[1]).resolve()
        self.assertTrue(report_path.is_relative_to((REPOSITORY_ROOT / "artifacts").resolve()))
        self.assertTrue(report_path.is_file())
        self.assertEqual(json.loads(report_path.read_text(encoding="utf-8"))["mode"], "book-audit")

        report_path.unlink(missing_ok=True)


class MoveAnnotationTests(unittest.TestCase):
    def test_win_probability_and_accuracy_curves(self):
        self.assertAlmostEqual(elo_oracle.win_probability_from_cp(0), 0.5, places=6)
        self.assertGreater(
            elo_oracle.win_probability_from_cp(300),
            elo_oracle.win_probability_from_cp(0),
        )
        self.assertEqual(elo_oracle.win_probability_from_eval(0, mate=2), 1.0)
        self.assertEqual(elo_oracle.win_probability_from_eval(0, mate=-1), 0.0)
        self.assertEqual(elo_oracle.move_accuracy_percent(0.5, 0.5), 100.0)
        self.assertLess(elo_oracle.move_accuracy_percent(0.6, 0.4), 50.0)

    def test_classify_move_annotation_covers_all_eleven_categories(self):
        self.assertEqual(
            elo_oracle.classify_move_annotation(0, 0, legal_move_count=1),
            "forced",
        )
        self.assertEqual(
            elo_oracle.classify_move_annotation(20, 0, is_book_move=True),
            "book",
        )
        self.assertEqual(
            elo_oracle.classify_move_annotation(
                60,
                60,
                second_best_cp=10,
                played_move="b5f7",
                best_move="b5f7",
                is_sacrifice=True,
            ),
            "brilliant",
        )
        self.assertEqual(
            elo_oracle.classify_move_annotation(
                120,
                120,
                second_best_cp=-40,
                played_move="e2e4",
                best_move="e2e4",
            ),
            "great",
        )
        self.assertEqual(
            elo_oracle.classify_move_annotation(
                30,
                30,
                second_best_cp=20,
                played_move="e2e4",
                best_move="e2e4",
            ),
            "best",
        )
        self.assertEqual(
            elo_oracle.classify_move_annotation(30, 18, played_move="d2d4", best_move="e2e4"),
            "excellent",
        )
        self.assertEqual(
            elo_oracle.classify_move_annotation(30, -5, played_move="c2c4", best_move="e2e4"),
            "good",
        )
        self.assertEqual(elo_oracle.classify_move_annotation(0, -65), "inaccuracy")
        self.assertEqual(elo_oracle.classify_move_annotation(0, -150), "mistake")
        self.assertEqual(elo_oracle.classify_move_annotation(400, 50), "miss")
        self.assertEqual(elo_oracle.classify_move_annotation(550, 50), "miss")
        self.assertEqual(
            elo_oracle.classify_move_annotation(0, 500, best_mate=2, played_mate=None),
            "miss",
        )
        self.assertEqual(
            elo_oracle.classify_move_annotation(
                0, 2, best_mate=2, played_mate=2, second_best_cp=480, played_move="d1h5", best_move="d1h5"
            ),
            "great",
        )
        self.assertNotEqual(
            elo_oracle.classify_move_annotation(
                40,
                40,
                second_best_cp=-150,
                played_move="e2e4",
                best_move="e2e4",
                is_sacrifice=True,
                is_capture_move=True,
                in_check=True,
            ),
            "brilliant",
        )
        self.assertEqual(
            elo_oracle.classify_move_annotation(
                20,
                20,
                second_best_cp=-400,
                played_move="f3e4",
                best_move="f3e4",
                is_sacrifice=True,
                is_capture_move=True,
            ),
            "great",
        )
        self.assertEqual(
            elo_oracle.classify_move_annotation(
                600,
                600,
                second_best_cp=520,
                played_move="b5f7",
                best_move="b5f7",
                is_sacrifice=True,
            ),
            "best",
        )
        self.assertEqual(elo_oracle.classify_move_annotation(0, -350), "blunder")
        self.assertEqual(
            elo_oracle.classify_move_annotation(0, 0, best_mate=None, played_mate=-2),
            "blunder",
        )

    def test_annotate_move_eval_and_pgn_formatting(self):
        annotated = elo_oracle.annotate_move_eval(0, -350)
        self.assertEqual(annotated["annotation"], "blunder")
        self.assertEqual(annotated["glyph"], "??")
        self.assertEqual(annotated["nag"], 4)
        self.assertEqual(annotated["cpl"], 350)
        self.assertLess(annotated["accuracy_percent"], 40.0)
        self.assertEqual(
            elo_oracle.format_pgn_move_annotation(
                "g4",
                "blunder",
                comment="[blunder] [%eval #-1]",
                include_nag=True,
                include_glyph=True,
            ),
            "g4?? $4 {[blunder] [%eval #-1]}",
        )

    def test_annotate_pgn_parsers_and_cli_with_fake_koi(self):
        parsed_move = annotate_pgn.parse_annotate_move_line(
            "info string annotate move g2g4 ply 2 side white best d2d4 classification blunder "
            "cpl 99990 accuracy 0.0 wp_before 0.450 wp_after 0.000 eval mate -1 best_eval cp -20 "
            "depth 3 glyph ?? nag 4 pv d2d4 d7d5"
        )
        self.assertIsNotNone(parsed_move)
        assert parsed_move is not None
        self.assertEqual(parsed_move["classification"], "blunder")
        self.assertEqual(parsed_move["eval_mate"], -1)
        self.assertEqual(parsed_move["nag"], 4)
        self.assertEqual(parsed_move["pv"], ["d2d4", "d7d5"])
        self.assertEqual(annotate_pgn.format_pgn_eval_tag(0, -1, "white"), "#-1")
        self.assertEqual(annotate_pgn.format_pgn_eval_tag(125, None, "black"), "-1.25")

        fake_koi_source = textwrap.dedent(
            """
            import sys
            for raw in sys.stdin:
                line = raw.strip()
                if line == "uci":
                    print("id name Fake Koi 1.1.5", flush=True)
                    print("uciok", flush=True)
                elif line == "isready":
                    print("readyok", flush=True)
                elif line.startswith("annotate game"):
                    print("info string annotate move f2f3 ply 0 side white best e2e4 classification inaccuracy cpl 75 accuracy 72.1 wp_before 0.520 wp_after 0.450 eval cp -55 best_eval cp 20 depth 3 glyph ?! nag 6 pv e2e4 e7e5", flush=True)
                    print("info string annotate move e7e5 ply 1 side black best e7e5 classification best cpl 0 accuracy 100.0 wp_before 0.550 wp_after 0.550 eval cp 55 best_eval cp 55 depth 3 pv e7e5", flush=True)
                    print("info string annotate move g2g4 ply 2 side white best d2d4 classification blunder cpl 99900 accuracy 0.0 wp_before 0.450 wp_after 0.000 eval mate -1 best_eval cp -40 depth 3 glyph ?? nag 4 pv d2d4 d7d5", flush=True)
                    print("info string annotate move d8h4 ply 3 side black best d8h4 classification great cpl 0 accuracy 100.0 wp_before 1.000 wp_after 1.000 eval mate 1 best_eval mate 1 depth 3 glyph ! nag 1 pv d8h4", flush=True)
                    print("info string annotate summary white moves 2 accuracy 36.0 avg_cpl 49987.5 brilliant 0 great 0 best 0 excellent 0 good 0 book 0 forced 0 inaccuracy 1 mistake 0 miss 0 blunder 1", flush=True)
                    print("info string annotate summary black moves 2 accuracy 100.0 avg_cpl 0.0 brilliant 0 great 1 best 1 excellent 0 good 0 book 0 forced 0 inaccuracy 0 mistake 0 miss 0 blunder 0", flush=True)
                elif line == "quit":
                    break
            """
        )

        with tempfile.TemporaryDirectory(prefix="koi annotate ") as temporary_directory:
            tmp = Path(temporary_directory)
            script = tmp / "fake_koi_annot.py"
            script.write_text(fake_koi_source, encoding="utf-8")
            if os.name == "nt":
                engine_cmd = tmp / "fake_koi.cmd"
                engine_cmd.write_text(f'@"{sys.executable}" "{script}"\n', encoding="utf-8")
            else:
                engine_cmd = tmp / "fake_koi.sh"
                engine_cmd.write_text(
                    f'#!/bin/sh\nexec "{sys.executable}" "{script}"\n',
                    encoding="utf-8",
                )
                engine_cmd.chmod(0o755)
            pgn_path = tmp / "game.pgn"
            out_pgn = tmp / "annotated.pgn"
            out_json = tmp / "annotated.json"
            pgn_path.write_text('[Result "0-1"]\n\n1. f3 e5 2. g4 Qh4# 0-1\n', encoding="utf-8")

            exit_code = annotate_pgn.main(
                [
                    "--pgn",
                    str(pgn_path),
                    "--koi",
                    str(engine_cmd),
                    "--depth",
                    "3",
                    "--output-pgn",
                    str(out_pgn),
                    "--output-json",
                    str(out_json),
                ]
            )
            self.assertEqual(exit_code, 0)
            pgn_text = out_pgn.read_text(encoding="utf-8")
            payload = json.loads(out_json.read_text(encoding="utf-8"))

        self.assertIn('[WhiteAccuracy "36.0%"]', pgn_text)
        self.assertIn('[BlackAccuracy "100.0%"]', pgn_text)
        self.assertIn("$6", pgn_text)
        self.assertIn("$4", pgn_text)
        self.assertIn("[%eval #-1]", pgn_text)
        self.assertEqual(payload["game_count"], 1)
        self.assertEqual(payload["games"][0]["white_summary"]["blunder"], 1)
        self.assertEqual(payload["games"][0]["moves"][2]["classification"], "blunder")


if __name__ == "__main__":
    unittest.main()
