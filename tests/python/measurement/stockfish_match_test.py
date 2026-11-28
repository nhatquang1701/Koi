import contextlib
import io
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tests" / "support"))
import unittest_progress  # noqa: E402,F401

from tools.measurement import book_audit, stockfish_match


class StockfishMatchOptionsTests(unittest.TestCase):
    def setUp(self):
        self.parser = stockfish_match._build_parser()
        self.base = ["--koi", r"C:\Koi\koi.exe", "--stockfish", r"C:\Stockfish\stockfish.exe"]

    def test_defaults_use_documented_measurement_settings(self):
        command = stockfish_match.build_command(self.parser.parse_args(self.base), powershell="pwsh-test")
        self.assertEqual(command[command.index("-Hash") + 1], "512")
        self.assertEqual(command[command.index("-KoiBookRandom") + 1], "false")
        self.assertNotIn("-OpponentElo", command)

    def test_book_random_and_explicit_opponent_elo_are_forwarded_and_clamped(self):
        args = self.parser.parse_args(self.base + ["--book-random", "true", "--opponent-elo", "0"])
        command = stockfish_match.build_command(args, powershell="pwsh-test")
        self.assertEqual(command[command.index("-KoiBookRandom") + 1], "true")
        self.assertEqual(command[command.index("-OpponentElo") + 1], "1320")

    def test_explicit_opponent_elo_is_forwarded(self):
        args = self.parser.parse_args(self.base + ["--opponent-elo", "2100"])
        command = stockfish_match.build_command(args, powershell="pwsh-test")
        self.assertEqual(command[command.index("-OpponentElo") + 1], "2100")

    def test_invalid_book_random_and_opponent_elo_are_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.parser.parse_args(self.base + ["--book-random", "maybe"])
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.parser.parse_args(self.base + ["--opponent-elo", "not-an-elo"])

    def test_operator_documentation_covers_strength_aliases_clamping_and_omission(self):
        documentation = (Path(__file__).resolve().parents[3] / "tools" / "README.md").read_text(encoding="utf-8")
        self.assertIn("`--opponent-elo`", documentation)
        self.assertIn("`--stockfish-elo`", documentation)
        self.assertIn("`-OpponentElo`", documentation)
        self.assertIn("`-StockfishElo`", documentation)
        self.assertIn("1320..3190", documentation)
        self.assertRegex(documentation, r"(?is)omit.*strength limiting.*disabled")

    def test_paired_run_metadata_is_forwarded_to_the_uci_harness(self):
        args = self.parser.parse_args(
            self.base + ["--batch-id", "batch-03", "--run-label", "before"]
        )
        command = stockfish_match.build_command(args, powershell="pwsh-test")
        self.assertEqual(command[command.index("-BatchId") + 1], "batch-03")
        self.assertEqual(command[command.index("-RunLabel") + 1], "before")

    def test_evaluator_flags_and_paired_colors_are_forwarded_and_validated(self):
        default_cmd = stockfish_match.build_command(
            self.parser.parse_args(self.base), powershell="pwsh-test"
        )
        self.assertNotIn("-KoiEvalFile", default_cmd)
        self.assertNotIn("-KoiEvaluatorMode", default_cmd)
        self.assertNotIn("-PairedColors", default_cmd)

        args = self.parser.parse_args(
            self.base
            + [
                "--koi-eval-file",
                "artifacts/networks/koi.nnue",
                "--koi-evaluator-mode",
                "nnue-v9",
                "--paired-colors",
            ]
        )
        command = stockfish_match.build_command(args, powershell="pwsh-test")
        self.assertEqual(
            command[command.index("-KoiEvalFile") + 1],
            str(Path("artifacts/networks/koi.nnue")),
        )
        self.assertEqual(command[command.index("-KoiEvaluatorMode") + 1], "nnue-v9")
        self.assertIn("-PairedColors", command)

        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.parser.parse_args(self.base + ["--koi-evaluator-mode", "nnue-v99"])

    def test_book_depth_rejects_negative_values_and_book_audit_rejects_conflicting_evaluator_flags(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.parser.parse_args(self.base + ["--book-depth", "-1"])

        args_zero = self.parser.parse_args(self.base + ["--book-depth", "0"])
        command_zero = stockfish_match.build_command(args_zero, powershell="pwsh-test")
        self.assertEqual(command_zero[command_zero.index("-KoiBookDepth") + 1], "0")

        audit_parser = book_audit._build_parser()
        audit_base = [
            "--pgn",
            "games.pgn",
            "--koi",
            r"C:\Koi\koi.exe",
            "--stockfish",
            r"C:\Stockfish\stockfish.exe",
            "--book",
            "book.bin",
        ]
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            audit_parser.parse_args(audit_base + ["--koi-eval-file", "net.nnue", "--hce"])


if __name__ == "__main__":
    unittest.main()
