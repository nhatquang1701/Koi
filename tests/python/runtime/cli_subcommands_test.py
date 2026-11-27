"""Cross-platform process tests for Koi CLI subcommands and diagnostics."""

from __future__ import annotations

import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "support"))
import unittest_progress  # noqa: E402,F401


def _resolve_engine() -> pathlib.Path:
    env_exe = os.environ.get("KOI_ENGINE_EXE")
    if env_exe:
        path = pathlib.Path(env_exe)
        if path.is_file():
            return path
    candidates = [
        ROOT / "build" / "loop-win" / "koi-engine.exe",
        ROOT / "build" / "windows-release" / "koi-engine.exe",
        ROOT / "build" / "release" / "koi-engine.exe",
        ROOT / "build" / "release" / "koi-engine",
        ROOT / "build" / "linux-gcc-release" / "koi-engine",
        ROOT / "build" / "macos-arm64-release" / "koi-engine",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise unittest.SkipTest("No built koi-engine executable found; set KOI_ENGINE_EXE")


def _resolve_tool(env_name: str, binary_stem: str, engine: pathlib.Path) -> pathlib.Path:
    env_exe = os.environ.get(env_name)
    if env_exe:
        path = pathlib.Path(env_exe)
        if path.is_file():
            return path
    suffix = engine.suffix
    sibling = engine.parent / f"{binary_stem}{suffix}"
    if sibling.is_file():
        return sibling
    raise unittest.SkipTest(f"No built {binary_stem} executable found; set {env_name}")


class CliSubcommandsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.engine = _resolve_engine()

    def _run(
        self,
        args: list[str],
        *,
        disable_nnue: bool = False,
        extra_env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        if disable_nnue:
            env.pop("KOI_NNUE_PATH", None)
            env.pop("KOI_EVALUATOR", None)
            env["KOI_NNUE_AUTODETECT"] = "0"
        if extra_env:
            env.update(extra_env)
        return subprocess.run(
            [str(self.engine), *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=env,
            timeout=30,
        )

    def test_version_help_and_engine_json(self) -> None:
        for flag in ("version", "--version", "-v"):
            proc = self._run([flag], disable_nnue=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertTrue(proc.stdout.startswith("Koi "), proc.stdout)

        for flag in ("help", "--help", "-h"):
            proc = self._run([flag], disable_nnue=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn("Usage: koi-engine", proc.stdout)
            self.assertIn("eval, --eval [fen]", proc.stdout)
            self.assertIn("perft <depth> [fen]", proc.stdout)

        proc = self._run(["--engine-json"], disable_nnue=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        payload = json.loads(proc.stdout)
        self.assertEqual(payload["name"], "Koi")
        self.assertTrue(payload["version"])
        self.assertEqual(
            {entry["name"]: entry["value"] for entry in payload["settings"]},
            {"Threads": 1, "Hash": 512},
        )

        expected_threads = min(2, os.cpu_count() or 1)
        for args, expected_hash in (
            (["--threads", str(expected_threads), "--hash", "256", "--engine-json"], 256),
            (["--engine-json", "--hash=128", f"--threads={expected_threads}"], 128),
        ):
            custom = self._run(args, disable_nnue=True)
            self.assertEqual(custom.returncode, 0, custom.stderr)
            custom_payload = json.loads(custom.stdout)
            self.assertEqual(
                {entry["name"]: entry["value"] for entry in custom_payload["settings"]},
                {"Threads": expected_threads, "Hash": expected_hash},
            )

        bad_json = self._run(["--engine-json", "--hash", "0"], disable_nnue=True)
        self.assertEqual(bad_json.returncode, 2, bad_json.stdout)
        self.assertIn("invalid hash", bad_json.stderr)

    def test_compiler_and_perft_boot_without_network(self) -> None:
        comp = self._run(["compiler"], disable_nnue=True)
        self.assertEqual(comp.returncode, 0, comp.stderr)
        self.assertIn("C++ standard   : C++26", comp.stdout)
        self.assertIn("SIMD / ISA     :", comp.stdout)

        perft3 = self._run(["perft", "3"], disable_nnue=True)
        self.assertEqual(perft3.returncode, 0, perft3.stderr)
        self.assertIn("Nodes searched: 8902", perft3.stdout)

        kiwipete = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"
        quoted = self._run(["perft", "2", kiwipete], disable_nnue=True)
        self.assertEqual(quoted.returncode, 0, quoted.stderr)
        self.assertIn("Nodes searched: 2039", quoted.stdout)

        unquoted = self._run(["perft", "2", *kiwipete.split()], disable_nnue=True)
        self.assertEqual(unquoted.returncode, 0, unquoted.stderr)
        self.assertIn("Nodes searched: 2039", unquoted.stdout)

    def test_position_inspection_subcommands_without_network(self) -> None:
        disp = self._run(["d"], disable_nnue=True)
        self.assertEqual(disp.returncode, 0, disp.stderr)
        self.assertIn("Moves : 20", disp.stdout)
        self.assertIn("State : ongoing", disp.stdout)

        mate_disp = self._run(["d", "7k/6Q1/6K1/8/8/8/8/8 b - - 0 1"], disable_nnue=True)
        self.assertEqual(mate_disp.returncode, 0, mate_disp.stderr)
        self.assertIn("Moves : 0", mate_disp.stdout)
        self.assertIn("State : checkmate (white wins)", mate_disp.stdout)

        kiwipete = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"
        disp_unquoted = self._run(["d", *kiwipete.split()], disable_nnue=True)
        self.assertEqual(disp_unquoted.returncode, 0, disp_unquoted.stderr)
        self.assertIn(kiwipete, disp_unquoted.stdout)
        self.assertIn("Moves : 48", disp_unquoted.stdout)

        key_out = self._run(["key"], disable_nnue=True)
        self.assertEqual(key_out.returncode, 0, key_out.stderr)
        self.assertIn("Position key :", key_out.stdout)
        self.assertIn("Pawn key     :", key_out.stdout)
        self.assertIn("Polyglot key :", key_out.stdout)

        moves_out = self._run(["moves"], disable_nnue=True)
        self.assertEqual(moves_out.returncode, 0, moves_out.stderr)
        self.assertIn("Legal moves (20): a2a3 ", moves_out.stdout)

        mate_moves = self._run(["moves", "8/8/8/8/8/6K1/5Q2/7k b - - 0 1"], disable_nnue=True)
        self.assertEqual(mate_moves.returncode, 0, mate_moves.stderr)
        self.assertEqual(mate_moves.stdout.strip(), "Legal moves (0): (none)")

        for cmd in ("d", "fen", "key", "moves", "flip"):
            bad = self._run([cmd, "bad fen"], disable_nnue=True)
            self.assertEqual(bad.returncode, 2)
            self.assertIn("invalid FEN", bad.stderr)

        fen_start = self._run(["fen"], disable_nnue=True)
        self.assertEqual(fen_start.returncode, 0, fen_start.stderr)
        self.assertEqual(
            fen_start.stdout.strip(),
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        )

        fen_moves = self._run(["fen", "moves", "e2e4", "e7e5"], disable_nnue=True)
        self.assertEqual(fen_moves.returncode, 0, fen_moves.stderr)
        self.assertEqual(
            fen_moves.stdout.strip(),
            "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",
        )

        flip_start = self._run(["flip"], disable_nnue=True)
        self.assertEqual(flip_start.returncode, 0, flip_start.stderr)
        self.assertEqual(
            flip_start.stdout.strip(),
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1",
        )

        flip_ep = self._run(
            ["flip", "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1"],
            disable_nnue=True,
        )
        self.assertEqual(flip_ep.returncode, 0, flip_ep.stderr)
        self.assertEqual(
            flip_ep.stdout.strip(),
            "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 1",
        )

        flip_check = self._run(["flip", "4k3/8/8/8/8/8/4R3/4K3 b - - 0 1"], disable_nnue=True)
        self.assertEqual(flip_check.returncode, 2)
        self.assertIn("flip rejected", flip_check.stderr)

        see_quiet = self._run(["see", "e2e4"], disable_nnue=True)
        self.assertEqual(see_quiet.returncode, 0, see_quiet.stderr)
        self.assertEqual(see_quiet.stdout.strip(), "SEE e2e4 : +0 cp")

        see_fen = "4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1"
        see_cap = self._run(["see", "e4d5", see_fen], disable_nnue=True)
        self.assertEqual(see_cap.returncode, 0, see_cap.stderr)
        self.assertEqual(see_cap.stdout.strip(), "SEE e4d5 : +100 cp")

        see_unquoted = self._run(["see", "e4d5", *see_fen.split()], disable_nnue=True)
        self.assertEqual(see_unquoted.returncode, 0, see_unquoted.stderr)
        self.assertEqual(see_unquoted.stdout.strip(), "SEE e4d5 : +100 cp")

        see_missing = self._run(["see"], disable_nnue=True)
        self.assertEqual(see_missing.returncode, 2)

        see_illegal = self._run(["see", "e1e8"], disable_nnue=True)
        self.assertEqual(see_illegal.returncode, 2)
        self.assertIn("illegal move", see_illegal.stderr)

        see_bad_fen = self._run(["see", "e2e4", "bad fen"], disable_nnue=True)
        self.assertEqual(see_bad_fen.returncode, 2)
        self.assertIn("invalid FEN", see_bad_fen.stderr)

        d_moves = self._run(["d", "moves", "e2e4", "e7e5"], disable_nnue=True)
        self.assertEqual(d_moves.returncode, 0, d_moves.stderr)
        self.assertIn(
            "Fen   : rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",
            d_moves.stdout,
        )

        perft_moves = self._run(["perft", "1", "moves", "e2e4"], disable_nnue=True)
        self.assertEqual(perft_moves.returncode, 0, perft_moves.stderr)
        self.assertIn("Nodes searched: 20", perft_moves.stdout)

        bad_moves = self._run(["d", "moves", "e2e5"], disable_nnue=True)
        self.assertEqual(bad_moves.returncode, 2, bad_moves.stdout)
        self.assertIn("illegal move", bad_moves.stderr)

    def test_invalid_perft_and_unknown_options(self) -> None:
        bad_depth = self._run(["perft", "99"], disable_nnue=True)
        self.assertEqual(bad_depth.returncode, 2)
        self.assertIn("invalid perft depth", bad_depth.stderr)

        bad_fen = self._run(["perft", "1", "bad fen"], disable_nnue=True)
        self.assertEqual(bad_fen.returncode, 2)
        self.assertIn("invalid FEN", bad_fen.stderr)

        unknown = self._run(["--definitely-unknown-flag"], disable_nnue=True)
        self.assertEqual(unknown.returncode, 2)
        self.assertIn("unknown option", unknown.stderr)

    def test_eval_and_bench_subcommands(self) -> None:
        extra = None
        if not os.environ.get("KOI_NNUE_PATH"):
            default_net = ROOT / "artifacts" / "networks" / "koi-v9-hm10-h256-official.nnue"
            if default_net.is_file():
                extra = {"KOI_NNUE_PATH": str(default_net)}
            else:
                extra = {"KOI_EVALUATOR": "hce"}

        for args in (
            ["eval"],
            ["eval", "4k3/8/8/8/8/8/4Q3/4K3 w - - 0 1"],
        ):
            proc = self._run(args, extra_env=extra)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            for label in (
                "Evaluator      :",
                "Side to move   :",
                "White relative :",
                "WDL (stm)      :",
                "Game phase     :",
            ):
                self.assertIn(label, proc.stdout)
            if extra != {"KOI_EVALUATOR": "hce"}:
                for nnue_label in (
                    "Network SHA256 :",
                    "Output bucket  :",
                    "Quantization   :",
                ):
                    self.assertIn(nnue_label, proc.stdout)

        bad_eval = self._run(["eval", "bad fen"], extra_env=extra)
        self.assertEqual(bad_eval.returncode, 2)
        self.assertIn("invalid FEN", bad_eval.stderr)

        cli_opts = self._run(["--hce", "--protocol", "xboard", "options"], disable_nnue=True)
        self.assertEqual(cli_opts.returncode, 0, cli_opts.stderr)
        self.assertEqual(cli_opts.stderr, "")
        self.assertIn("Protocol        : xboard", cli_opts.stdout)
        self.assertIn("Evaluator       : HCE", cli_opts.stdout)
        self.assertIn("Hash            : 512 MB", cli_opts.stdout)

        hce_eval = self._run(["eval"], extra_env={"KOI_EVALUATOR": "hce"})
        self.assertEqual(hce_eval.returncode, 0, hce_eval.stderr)
        self.assertIn("HCE (classical tapered)", hce_eval.stdout)
        self.assertIn("Total (White)", hce_eval.stdout)

        bench = self._run(["bench", "3", "8", "1"], extra_env=extra)
        self.assertEqual(bench.returncode, 0, bench.stderr)
        self.assertIn("Total time (ms) :", bench.stdout)
        self.assertIn("Nodes searched  :", bench.stdout)
        self.assertIn("Nodes/second    :", bench.stdout)

        speedtest = self._run(["--hce", "speedtest", "1", "8", "1"], disable_nnue=True)
        self.assertEqual(speedtest.returncode, 0, speedtest.stderr)
        self.assertEqual(speedtest.stderr, "")
        self.assertIn("Threads         : 1", speedtest.stdout)
        self.assertIn("Hash (MB)       : 8", speedtest.stdout)
        self.assertIn("Total time (ms) :", speedtest.stdout)
        self.assertIn("Nodes searched  :", speedtest.stdout)
        self.assertIn("Nodes/second    :", speedtest.stdout)

        speedtest_hash_flag = self._run(["--hce", "--hash", "24", "speedtest", "1"], disable_nnue=True)
        self.assertEqual(speedtest_hash_flag.returncode, 0, speedtest_hash_flag.stderr)
        self.assertEqual(speedtest_hash_flag.stderr, "")
        self.assertIn("Hash (MB)       : 24", speedtest_hash_flag.stdout)

        speedtest_pos_wins = self._run(
            ["--hce", "--hash", "64", "speedtest", "1", "16", "1"], disable_nnue=True
        )
        self.assertEqual(speedtest_pos_wins.returncode, 0, speedtest_pos_wins.stderr)
        self.assertEqual(speedtest_pos_wins.stderr, "")
        self.assertIn("Hash (MB)       : 16", speedtest_pos_wins.stdout)

        bad_speedtest = self._run(["--hce", "speedtest", "0"], disable_nnue=True)
        self.assertEqual(bad_speedtest.returncode, 2, bad_speedtest.stdout)
        self.assertIn("invalid speedtest threads", bad_speedtest.stderr)

    def test_hce_and_nnue_cli_flags(self) -> None:
        hce_eval = self._run(["--hce", "eval"], disable_nnue=True)
        self.assertEqual(hce_eval.returncode, 0, hce_eval.stderr)
        self.assertIn("HCE (classical tapered)", hce_eval.stdout)
        self.assertIn("Total (White)", hce_eval.stdout)

        hce_eval_moves = self._run(
            ["--hce", "eval", "startpos", "moves", "e2e4", "e7e5"],
            disable_nnue=True,
        )
        self.assertEqual(hce_eval_moves.returncode, 0, hce_eval_moves.stderr)
        self.assertIn("HCE (classical tapered)", hce_eval_moves.stdout)

        hce_bench = self._run(["--hce", "bench", "2", "8", "1"], disable_nnue=True)
        self.assertEqual(hce_bench.returncode, 0, hce_bench.stderr)
        self.assertIn("Nodes searched  :", hce_bench.stdout)

        nnue_path = os.environ.get("KOI_NNUE_PATH")
        if not nnue_path or not pathlib.Path(nnue_path).is_file():
            for candidate in (
                self.engine.parent / "koi.nnue",
                self.engine.parent / "nnue" / "koi.nnue",
                ROOT / "artifacts" / "networks" / "koi-v9-hm10-h256-official.nnue",
            ):
                if candidate.is_file():
                    nnue_path = str(candidate)
                    break
        self.assertTrue(
            nnue_path and pathlib.Path(nnue_path).is_file(),
            "Expected an NNUE network fixture for --nnue test",
        )
        nnue_eval = self._run(
            ["--nnue", str(nnue_path), "eval"],
            disable_nnue=True,
            extra_env={"KOI_EVALUATOR": "hce"},
        )
        self.assertEqual(nnue_eval.returncode, 0, nnue_eval.stderr)
        self.assertIn("Evaluator      : NNUE", nnue_eval.stdout)
        self.assertIn("Network SHA256 :", nnue_eval.stdout)

        nnue_info = self._run(["nnue", str(nnue_path)], disable_nnue=True)
        self.assertEqual(nnue_info.returncode, 0, nnue_info.stderr)
        self.assertEqual(nnue_info.stderr, "")
        for expected_field in (
            "Format version : v",
            "Feature set    :",
            "Architecture   :",
            "Payload SHA256 :",
            "Status         : valid (checksum verified)",
        ):
            self.assertIn(expected_field, nnue_info.stdout)

        with tempfile.TemporaryDirectory() as tmp_dir:
            corrupt = pathlib.Path(tmp_dir) / "corrupt.nnue"
            corrupt.write_bytes(b"not-a-net")
            bad_nnue_info = self._run(["nnue", str(corrupt)], disable_nnue=True)
            self.assertEqual(bad_nnue_info.returncode, 2, bad_nnue_info.stdout)
            self.assertIn("invalid NNUE network", bad_nnue_info.stderr)

        conflict = self._run(["--hce", "--nnue", str(nnue_path), "eval"], disable_nnue=True)
        self.assertEqual(conflict.returncode, 2)
        self.assertIn("cannot be combined", conflict.stderr)

        missing_nnue = self._run(["--nnue"], disable_nnue=True)
        self.assertEqual(missing_nnue.returncode, 2)
        self.assertIn("missing path for --nnue", missing_nnue.stderr)

    def test_book_and_tb_cli_subcommands_without_network(self) -> None:
        key_proc = self._run(["key"], disable_nnue=True)
        self.assertEqual(key_proc.returncode, 0, key_proc.stderr)
        polyglot_key = None
        for line in key_proc.stdout.splitlines():
            if line.startswith("Polyglot key : "):
                polyglot_key = int(line.split(":", 1)[1].strip(), 16)
                break
        self.assertIsNotNone(polyglot_key, key_proc.stdout)

        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = pathlib.Path(tmp_dir)
            book_file = tmp_path / "book.bin"
            # Polyglot move encoding: (from << 6) | to; e2=12, e4=28.
            book_file.write_bytes(struct.pack(">QHHI", polyglot_key, (12 << 6) | 28, 100, 0))
            book_ok = self._run(["--book", str(book_file), "book"], disable_nnue=True)
            self.assertEqual(book_ok.returncode, 0, book_ok.stderr)
            self.assertIn("Opening book : book.bin", book_ok.stdout)
            self.assertIn("e2e4  weight=100 (100.0%)", book_ok.stdout)

            missing_book = self._run(["--book", str(tmp_path / "missing.bin"), "book"], disable_nnue=True)
            self.assertEqual(missing_book.returncode, 0, missing_book.stderr)
            self.assertEqual(missing_book.stdout.strip(), "Opening book : unavailable")

            tb_empty = self._run(["--syzygy", str(tmp_path), "tb"], disable_nnue=True)
            self.assertEqual(tb_empty.returncode, 0, tb_empty.stderr)
            self.assertEqual(tb_empty.stdout.strip(), "Syzygy : unavailable")

            book_opts = self._run(["--hce", "--book", str(book_file), "options"], disable_nnue=True)
            self.assertEqual(book_opts.returncode, 0, book_opts.stderr)
            self.assertEqual(book_opts.stderr, "")
            self.assertIn("OwnBook         : true (explicit: true, available: true,", book_opts.stdout)
            self.assertIn("book.bin", book_opts.stdout)

            book_batch = self._run(["--hce", "--book", str(book_file), "-c", "book"], disable_nnue=True)
            self.assertEqual(book_batch.returncode, 0, book_batch.stderr)
            self.assertEqual(book_batch.stderr, "")
            self.assertIn("Opening book : book.bin", book_batch.stdout)
            self.assertIn("e2e4  weight=100 (100.0%)", book_batch.stdout)

            syz_opts = self._run(["--hce", "--syzygy", str(tmp_path), "options"], disable_nnue=True)
            self.assertEqual(syz_opts.returncode, 0, syz_opts.stderr)
            self.assertEqual(syz_opts.stderr, "")
            self.assertIn(f"SyzygyPath      : {tmp_path}", syz_opts.stdout)
            self.assertNotIn("info string", syz_opts.stdout)

        for cmd in ("book", "tb"):
            bad_fen = self._run([cmd, "bad fen"], disable_nnue=True)
            self.assertEqual(bad_fen.returncode, 2)
            self.assertIn("invalid FEN", bad_fen.stderr)

        for flag in ("--book", "--syzygy"):
            missing = self._run([flag], disable_nnue=True)
            self.assertEqual(missing.returncode, 2)
            self.assertIn("missing path", missing.stderr)

    def test_inline_cmd_batch_execution(self) -> None:
        handshake = self._run(["--hce", "-c", "uci", "-c", "isready"], disable_nnue=True)
        self.assertEqual(handshake.returncode, 0, handshake.stderr)
        self.assertEqual(handshake.stderr, "")
        self.assertIn("id name Koi ", handshake.stdout)
        self.assertIn("uciok", handshake.stdout)
        self.assertIn("readyok", handshake.stdout)

        search = self._run(
            ["--hce", "-c", "position startpos moves e2e4 e7e5", "-c", "go depth 3"],
            disable_nnue=True,
        )
        self.assertEqual(search.returncode, 0, search.stderr)
        self.assertEqual(search.stderr, "")
        self.assertIn("info depth 3", search.stdout)
        self.assertIn("bestmove ", search.stdout)

        nnue_path = os.environ.get("KOI_NNUE_PATH")
        if not nnue_path or not pathlib.Path(nnue_path).is_file():
            for candidate in (
                self.engine.parent / "koi.nnue",
                self.engine.parent / "nnue" / "koi.nnue",
                ROOT / "artifacts" / "networks" / "koi-v9-hm10-h256-official.nnue",
            ):
                if candidate.is_file():
                    nnue_path = str(candidate)
                    break
        self.assertTrue(
            nnue_path and pathlib.Path(nnue_path).is_file(),
            "Expected an NNUE network fixture for --cmd NNUE test",
        )
        nnue_eval = self._run(
            ["--nnue", str(nnue_path), "--cmd=position startpos", "--cmd=eval", "--cmd=nnue", "--cmd=options"],
            disable_nnue=True,
            extra_env={"KOI_EVALUATOR": "hce"},
        )
        self.assertEqual(nnue_eval.returncode, 0, nnue_eval.stderr)
        self.assertEqual(nnue_eval.stderr, "")
        nnue_name = pathlib.Path(nnue_path).name
        self.assertIn("Evaluator      : NNUE", nnue_eval.stdout)
        self.assertIn("Network SHA256 :", nnue_eval.stdout)
        self.assertIn(f"NNUE file      : {nnue_name}", nnue_eval.stdout)
        self.assertIn("Format version : v", nnue_eval.stdout)
        self.assertIn("Payload SHA256 :", nnue_eval.stdout)
        self.assertIn("Status         : valid (checksum verified)", nnue_eval.stdout)
        self.assertIn(f"EvalFile        : {nnue_name} (boot)", nnue_eval.stdout)

        hce_nnue = self._run(["--hce", "-c", "nnue"], disable_nnue=True)
        self.assertEqual(hce_nnue.returncode, 0, hce_nnue.stderr)
        self.assertEqual(hce_nnue.stderr, "")
        self.assertIn("NNUE : inactive (EvalMode=hce)", hce_nnue.stdout)

        makemove_undo = self._run(
            ["--hce", "-c", "makemove e2e4 e7e5", "-c", "fen", "-c", "undo", "-c", "fen"],
            disable_nnue=True,
        )
        self.assertEqual(makemove_undo.returncode, 0, makemove_undo.stderr)
        self.assertEqual(makemove_undo.stderr, "")
        self.assertIn(
            "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",
            makemove_undo.stdout,
        )
        self.assertIn(
            "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1",
            makemove_undo.stdout,
        )

        xboard = self._run(
            ["--protocol", "xboard", "--hce", "-c", "protover 2", "-c", "sd 2", "-c", "go"],
            disable_nnue=True,
        )
        self.assertEqual(xboard.returncode, 0, xboard.stderr)
        self.assertEqual(xboard.stderr, "")
        self.assertIn("feature ", xboard.stdout)
        self.assertIn("move ", xboard.stdout)

        live_options = self._run(
            [
                "--hce",
                "-c",
                "setoption name Hash value 32",
                "-c",
                "setoption name Contempt value 15",
                "-c",
                "options",
            ],
            disable_nnue=True,
        )
        self.assertEqual(live_options.returncode, 0, live_options.stderr)
        self.assertEqual(live_options.stderr, "")
        self.assertIn("Hash            : 32 MB", live_options.stdout)
        self.assertIn("Contempt        : 15 cp", live_options.stdout)
        self.assertIn("Evaluator       : HCE", live_options.stdout)

        cli_hash_threads = self._run(
            ["--hce", "--hash", "64", "--threads", "1", "options"],
            disable_nnue=True,
        )
        self.assertEqual(cli_hash_threads.returncode, 0, cli_hash_threads.stderr)
        self.assertEqual(cli_hash_threads.stderr, "")
        self.assertIn("Hash            : 64 MB", cli_hash_threads.stdout)
        self.assertIn("Threads         : 1", cli_hash_threads.stdout)

        inline_batch = self._run(
            ["--hce", "--hash=32", "--threads=1", "-c", "position startpos", "-c", "go depth 2"],
            disable_nnue=True,
        )
        self.assertEqual(inline_batch.returncode, 0, inline_batch.stderr)
        self.assertEqual(inline_batch.stderr, "")
        self.assertIn("bestmove ", inline_batch.stdout)

        for bad_args, expected_err in (
            (["-c"], "missing command for --cmd"),
            (["--cmd="], "missing command for --cmd"),
            (["--hce", "-c", "uci", "bench"], "cannot be combined"),
            (["--hash", "0"], "invalid hash"),
            (["--threads", "0"], "invalid threads"),
        ):
            bad = self._run(bad_args, disable_nnue=True)
            self.assertEqual(bad.returncode, 2, bad.stdout)
            self.assertIn(expected_err, bad.stderr)

    def test_developer_tool_cli_subcommands(self) -> None:
        perft_exe = _resolve_tool("KOI_PERFT_EXE", "koi-perft", self.engine)
        replay_exe = _resolve_tool("KOI_REPLAY_EXE", "koi-replay", self.engine)
        bench_exe = _resolve_tool("KOI_BENCH_EXE", "koi-bench", self.engine)

        help_perft = subprocess.run([str(perft_exe), "--help"], capture_output=True, text=True, timeout=15)
        self.assertEqual(help_perft.returncode, 0, help_perft.stderr)
        self.assertIn("usage: koi-perft", help_perft.stdout)

        perft_plain = subprocess.run([str(perft_exe), "3"], capture_output=True, text=True, timeout=15)
        self.assertEqual(perft_plain.returncode, 0, perft_plain.stderr)
        self.assertEqual(perft_plain.stdout.strip(), "nodes 8902")

        kiwipete = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"
        perft_div = subprocess.run(
            [str(perft_exe), "--divide", "2", kiwipete],
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(perft_div.returncode, 0, perft_div.stderr)
        self.assertIn("nodes 2039", perft_div.stdout)

        perft_unquoted = subprocess.run(
            [str(perft_exe), "--divide", "2", *kiwipete.split()],
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(perft_unquoted.returncode, 0, perft_unquoted.stderr)
        self.assertIn("nodes 2039", perft_unquoted.stdout)

        bad_fen = subprocess.run([str(perft_exe), "1", "bad fen"], capture_output=True, text=True, timeout=15)
        self.assertNotEqual(bad_fen.returncode, 0)
        self.assertIn("invalid FEN", bad_fen.stderr)

        perft_moves = subprocess.run([str(perft_exe), "1", "moves", "e2e4"], capture_output=True, text=True, timeout=15)
        self.assertEqual(perft_moves.returncode, 0, perft_moves.stderr)
        self.assertEqual(perft_moves.stdout.strip(), "nodes 20")

        help_replay = subprocess.run([str(replay_exe), "--help"], capture_output=True, text=True, timeout=15)
        self.assertEqual(help_replay.returncode, 0, help_replay.stderr)
        self.assertIn("usage: koi-replay", help_replay.stdout)

        replay_ok = subprocess.run(
            [str(replay_exe), "startpos", "moves", "e2e4", "e7e5"],
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(replay_ok.returncode, 0, replay_ok.stderr)
        self.assertIn("legal 1", replay_ok.stdout)
        self.assertIn("termination ongoing", replay_ok.stdout)

        help_bench = subprocess.run([str(bench_exe), "--help"], capture_output=True, text=True, timeout=15)
        self.assertEqual(help_bench.returncode, 0, help_bench.stderr)
        self.assertIn("usage: koi-bench", help_bench.stdout)

        eval_features_exe = _resolve_tool("KOI_EVAL_FEATURES_EXE", "koi-eval-features", self.engine)
        nnue_fixture_exe = _resolve_tool("KOI_NNUE_FIXTURE_EXE", "koi-nnue-fixture", self.engine)
        for tool_exe, expected_prefix in (
            (eval_features_exe, "usage: koi-eval-features"),
            (nnue_fixture_exe, "usage: koi-nnue-fixture"),
        ):
            for flag in ("-h", "--help"):
                proc = subprocess.run([str(tool_exe), flag], capture_output=True, text=True, timeout=15)
                self.assertEqual(proc.returncode, 0, f"{tool_exe} {flag}: {proc.stderr}")
                self.assertIn(expected_prefix, proc.stdout)

        for tool_exe, stem in (
            (perft_exe, "koi-perft"),
            (replay_exe, "koi-replay"),
            (bench_exe, "koi-bench"),
            (eval_features_exe, "koi-eval-features"),
            (nnue_fixture_exe, "koi-nnue-fixture"),
        ):
            for flag in ("-v", "--version"):
                proc = subprocess.run([str(tool_exe), flag], capture_output=True, text=True, timeout=15)
                self.assertEqual(proc.returncode, 0, f"{tool_exe} {flag}: {proc.stderr}")
                self.assertEqual(proc.stderr, "")
                line = proc.stdout.strip()
                self.assertTrue(line.startswith(f"{stem} "), f"{stem} {flag}: {line!r}")
                self.assertIn("(", line)
                self.assertIn(")", line)

        with tempfile.TemporaryDirectory() as tmpdir:
            v9_path = pathlib.Path(tmpdir) / "fixture-v9.nnue"
            gen = subprocess.run(
                [str(nnue_fixture_exe), "--arch", "v9", "--output", str(v9_path)],
                capture_output=True,
                text=True,
                timeout=15,
            )
            self.assertEqual(gen.returncode, 0, gen.stderr)
            inspected = self._run(["nnue", str(v9_path)], disable_nnue=True)
            self.assertEqual(inspected.returncode, 0, inspected.stderr)
            self.assertIn("Format version : v9", inspected.stdout)
            self.assertIn("Feature set    : halfka-hm-v2", inspected.stdout)
            self.assertIn("Status         : valid (checksum verified)", inspected.stdout)

            nnue_opts = self._run(["--nnue", str(v9_path), "options"], disable_nnue=True)
            self.assertEqual(nnue_opts.returncode, 0, nnue_opts.stderr)
            self.assertIn("Evaluator       : NNUE", nnue_opts.stdout)
            self.assertIn("fixture-v9.nnue (boot)", nnue_opts.stdout)


if __name__ == "__main__":
    unittest.main()
