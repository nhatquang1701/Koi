"""Cross-platform process tests for Koi CLI subcommands and diagnostics."""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
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
        for flag in ("--version", "-v"):
            proc = self._run([flag], disable_nnue=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertTrue(proc.stdout.startswith("Koi "), proc.stdout)

        for flag in ("--help", "-h"):
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

        for cmd in ("d", "key", "moves", "flip"):
            bad = self._run([cmd, "bad fen"], disable_nnue=True)
            self.assertEqual(bad.returncode, 2)
            self.assertIn("invalid FEN", bad.stderr)

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

        hce_eval = self._run(["eval"], extra_env={"KOI_EVALUATOR": "hce"})
        self.assertEqual(hce_eval.returncode, 0, hce_eval.stderr)
        self.assertIn("HCE (classical tapered)", hce_eval.stdout)
        self.assertIn("Total (White)", hce_eval.stdout)

        bench = self._run(["bench", "3", "8", "1"], extra_env=extra)
        self.assertEqual(bench.returncode, 0, bench.stderr)
        self.assertIn("Total time (ms) :", bench.stdout)
        self.assertIn("Nodes searched  :", bench.stdout)
        self.assertIn("Nodes/second    :", bench.stdout)

    def test_hce_and_nnue_cli_flags(self) -> None:
        hce_eval = self._run(["--hce", "eval"], disable_nnue=True)
        self.assertEqual(hce_eval.returncode, 0, hce_eval.stderr)
        self.assertIn("HCE (classical tapered)", hce_eval.stdout)
        self.assertIn("Total (White)", hce_eval.stdout)

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

        conflict = self._run(["--hce", "--nnue", str(nnue_path), "eval"], disable_nnue=True)
        self.assertEqual(conflict.returncode, 2)
        self.assertIn("cannot be combined", conflict.stderr)

        missing_nnue = self._run(["--nnue"], disable_nnue=True)
        self.assertEqual(missing_nnue.returncode, 2)
        self.assertIn("missing path for --nnue", missing_nnue.stderr)

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


if __name__ == "__main__":
    unittest.main()
