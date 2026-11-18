"""Unit tests for the Stockfish-verified Polyglot book builder."""

from __future__ import annotations

import pathlib
import struct
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
for extra in (ROOT / "tools" / "book", ROOT / "tools" / "measurement"):
    if str(extra) not in sys.path:
        sys.path.insert(0, str(extra))

import build_verified_book as builder
import koi_chess as chess
import polyglot


class VerifiedBookBuilderTest(unittest.TestCase):
    def test_polyglot_keys_match_engine_reference_vectors(self):
        start = chess.Board()
        self.assertEqual(polyglot.polyglot_key(start), 0x463B96181691FC9C)

        start.push(chess.Move.from_uci("e2e4"))
        self.assertEqual(polyglot.polyglot_key(start), 0x823C9B50FD114196)

        castling = chess.Board("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1")
        self.assertEqual(polyglot.polyglot_key(castling), 0xFDA239CC692A6053)
        self.assertEqual(
            polyglot.encode_move(castling, chess.Move.from_uci("e1g1")),
            (chess.parse_square("h1")) | (chess.parse_square("e1") << 6),
        )
        self.assertEqual(
            polyglot.encode_move(castling, chess.Move.from_uci("e1c1")),
            (chess.parse_square("a1")) | (chess.parse_square("e1") << 6),
        )

        ep_yes = chess.Board("rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2")
        ep_no = chess.Board("rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq - 0 2")
        self.assertEqual(polyglot.polyglot_key(ep_yes), 0x849991DFA8617835)
        self.assertEqual(polyglot.polyglot_key(ep_no), 0x4BA8D401A2BC3ABC)

        promo = chess.Board("4k3/P7/8/8/8/8/8/4K3 w - - 0 1")
        self.assertEqual(polyglot.polyglot_key(promo), 0xE7783B5A70DA7685)
        self.assertEqual(
            polyglot.encode_move(promo, chess.Move.from_uci("a7a8q")),
            chess.parse_square("a8") | (chess.parse_square("a7") << 6) | (4 << 12),
        )

    def test_minimax_prunes_popular_human_trap_and_handles_cycles(self):
        pgn_text = "\n".join(
            [
                '[Event "T"]\n[WhiteElo "2200"]\n[BlackElo "2200"]\n[TimeControl "300+0"]\n[Result "1-0"]\n\n1. e4 e5 2. Qh5 Nc6 *\n',
            ]
            * 10
            + [
                '[Event "T"]\n[WhiteElo "2200"]\n[BlackElo "2200"]\n[TimeControl "300+0"]\n[Result "1-0"]\n\n1. e4 e5 2. Qh5 Nc6 1-0\n',
            ]
            * 12
            + [
                '[Event "T"]\n[WhiteElo "2200"]\n[BlackElo "2200"]\n[TimeControl "300+0"]\n[Result "1/2-1/2"]\n\n1. e4 e5 2. Nf3 Nc6 1/2-1/2\n',
            ]
            * 4
            + [
                '[Event "T"]\n[WhiteElo "2200"]\n[BlackElo "2200"]\n[TimeControl "300+0"]\n[Result "1/2-1/2"]\n\n1. Nf3 Nf6 2. Ng1 Ng8 1/2-1/2\n',
            ]
            * 5
        )
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = pathlib.Path(tmpdir)
            pgn_path = tmp / "sample.pgn"
            pgn_path.write_text(pgn_text, encoding="utf-8")

            dag = builder.extract_candidate_tree(
                sources=[str(pgn_path)],
                seed_openings=[],
                min_elo=2000.0,
                min_opponent=1800,
                min_base_seconds=180,
                max_ply=8,
                min_count=2,
                max_nodes=100,
            )

            # Key lookup for tested positions
            b = chess.Board()
            k_start = polyglot.polyglot_key(b)
            b.push(chess.Move.from_uci("e2e4"))
            k_e4 = polyglot.polyglot_key(b)
            b.push(chess.Move.from_uci("e7e5"))
            k_e4e5 = polyglot.polyglot_key(b)

            b_trap = b.copy(stack=False)
            b_trap.push(chess.Move.from_uci("d1h5"))
            k_qh5 = polyglot.polyglot_key(b_trap)

            b_sound = b.copy(stack=False)
            b_sound.push(chess.Move.from_uci("g1f3"))
            k_nf3 = polyglot.polyglot_key(b_sound)

            # Static eval: Qh5 fails sharply (Black up +95 cp after d1h5); Nf3 is normal (-20 cp for Black -> +20 White)
            evals = {k: builder.EngineEval(eval_cp=0) for k in dag.fens}
            evals[k_start] = builder.EngineEval(eval_cp=25)
            evals[k_e4] = builder.EngineEval(eval_cp=-20)
            evals[k_e4e5] = builder.EngineEval(eval_cp=20)
            evals[k_qh5] = builder.EngineEval(eval_cp=95)
            evals[k_nf3] = builder.EngineEval(eval_cp=-20)

            records = builder.minimax_and_prune(
                dag, evals, tolerance_cp=25, min_abs_cp=-65, max_size_bytes=4096
            )
            e4e5_entries = [r for r in records if r[0] == k_e4e5]
            self.assertEqual(len(e4e5_entries), 1, "Dubious 2.Qh5 trap (-95 cp) must be pruned")
            self.assertEqual(
                e4e5_entries[0][1],
                polyglot.encode_move(chess.Board("rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2"), chess.Move.from_uci("g1f3")),
            )

            # Write and verify sorting & record alignment
            out_bin = tmp / "out.bin"
            data = polyglot.write_book(out_bin, records)
            self.assertEqual(len(data) % 16, 0)
            unpacked = [struct.unpack_from(">QHHI", data, off) for off in range(0, len(data), 16)]
            keys = [row[0] for row in unpacked]
            self.assertEqual(keys, sorted(keys))

    def test_budget_pruner_respects_max_size_bytes_and_reachability(self):
        start = chess.Board()
        pgn_lines = []
        for idx, open_mv in enumerate(["e4", "d4", "c4", "Nf3"]):
            for reply in ["e5", "c5", "d5", "Nf6"]:
                b = start.copy(stack=False)
                m1 = b.parse_san(open_mv)
                b.push(m1)
                try:
                    b.parse_san(reply)
                except ValueError:
                    continue
                pgn_lines.append(
                    f'[Event "E{idx}"]\n[WhiteElo "2300"]\n[BlackElo "2300"]\n[TimeControl "300+0"]\n[Result "1/2-1/2"]\n\n1. {open_mv} {reply} 2. Nc3 Nc6 1/2-1/2\n\n' * 4
                )
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = pathlib.Path(tmpdir)
            pgn = tmp / "wide.pgn"
            pgn.write_text("".join(pgn_lines), encoding="utf-8")
            dag = builder.extract_candidate_tree(
                sources=[str(pgn)],
                seed_openings=[],
                min_elo=2000.0,
                min_opponent=1800,
                min_base_seconds=180,
                max_ply=4,
                min_count=2,
                max_nodes=100,
            )
            evals = {k: builder.EngineEval(eval_cp=0) for k in dag.fens}
            records = builder.minimax_and_prune(
                dag, evals, tolerance_cp=30, min_abs_cp=-65, max_size_bytes=128
            )
            self.assertGreater(len(records), 0)
            self.assertLessEqual(len(records) * 16, 128)

    def test_scored_info_parser_ignores_trailing_strings_and_caps_bounds(self):
        self.assertIsNone(builder._parse_scored_info("info string NNUE evaluation using foo"))
        self.assertIsNone(builder._parse_scored_info("info depth 14 currmove e2e4 currmovenumber 1"))
        self.assertEqual(
            builder._parse_scored_info("info depth 14 seldepth 20 multipv 1 score cp 28 nodes 50000 pv e2e4 e7e5"),
            (28, "e2e4", False),
        )
        self.assertEqual(
            builder._parse_scored_info("info depth 15 score cp 120 upperbound pv d2d4"),
            (120, "d2d4", True),
        )

    def test_seed_openings_loads_eco_tsv_and_uci_lines(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = pathlib.Path(tmpdir)
            tsv = tmp / "openings.tsv"
            tsv.write_text(
                "eco\tname\tpgn\nB90\tSicilian Najdorf\t1. e4 c5 2. Nf3 d6 3. d4 cxd4 4. Nxd4 Nf6 5. Nc3 a6\n",
                encoding="utf-8",
            )
            uci_file = tmp / "uci.txt"
            uci_file.write_text("# comment\nd2d4 g8f6 c2c4 e7e6\n", encoding="utf-8")

            dag = builder.extract_candidate_tree(
                sources=[],
                seed_openings=[str(tsv), str(uci_file)],
                min_elo=2000.0,
                min_opponent=1800,
                min_base_seconds=180,
                max_ply=12,
                min_count=1,
                max_nodes=100,
            )
            start = chess.Board()
            root_edges = dag.edges[polyglot.polyglot_key(start)]
            self.assertIn("e2e4", root_edges)
            self.assertIn("d2d4", root_edges)
            self.assertTrue(root_edges["e2e4"].sf_seeded)


if __name__ == "__main__":
    unittest.main()
