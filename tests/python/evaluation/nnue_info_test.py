"""Tests for the NNUE container inspector."""

import hashlib
import importlib.util
import pathlib
import struct
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "support"))
import unittest_progress  # noqa: E402,F401
INFO_PATH = ROOT / "tools" / "measurement" / "nnue_info.py"
TRAINER_PATH = ROOT / "tools" / "measurement" / "train_nnue_koi.py"
HEADER = struct.Struct("<8sIIIIIBBBBHHQ")


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _build_container(version: int, dims: tuple[int, int, int, int], meta: bytes,
                     feature_set: bytes, payload: bytes,
                     quantization: bytes = b"int16/int8") -> bytes:
    header = HEADER.pack(
        b"KOI-NNUE",
        version,
        dims[0], dims[1], dims[2], dims[3],
        meta[0], meta[1], meta[2], meta[3],
        len(quantization),
        len(feature_set),
        len(payload),
    )
    digest = hashlib.sha256(payload).digest()
    return header + digest + quantization + feature_set + payload


class NnueInfoTest(unittest.TestCase):
    def setUp(self):
        self.info = _load(INFO_PATH, "nnue_info")
        self.trainer = _load(TRAINER_PATH, "train_nnue_koi_info")

    def test_rejects_non_container_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "bad.nnue"
            path.write_bytes(b"not-a-network" * 4)
            with self.assertRaises(self.info.NnueInfoError):
                self.info.read_container(path)

    def test_reads_v9_halfka_hm_container(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "v9.nnue"
            payload = b"\x01\x02\x03\x04" * 8
            container = _build_container(
                9,
                (7680, 256, 8, 1),
                struct.pack("<BBH", 255, 64, 400),
                b"halfka-hm-v2",
                payload,
            )
            path.write_bytes(container)
            parsed = self.info.read_container(path)
            self.assertEqual(parsed["version"], 9)
            self.assertEqual(parsed["input_units"], 7680)
            self.assertEqual(parsed["hidden"], 256)
            self.assertEqual(parsed["output_buckets"], 8)
            self.assertEqual(parsed["qa"], 255)
            self.assertEqual(parsed["qb"], 64)
            self.assertEqual(parsed["eval_scale"], 400)
            self.assertEqual(parsed["feature_set"], "halfka-hm-v2")
            self.assertTrue(parsed["checksum_valid"])
            self.assertEqual(parsed["file_sha256"], hashlib.sha256(container).hexdigest())
            self.assertNotIn("hidden_shift", parsed)

    def test_reads_v4_dual_perspective_container(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "v4.nnue"
            container = _build_container(
                4,
                (9216, 128, 8, 2),
                bytes([6, 4, 0, 0]),
                b"halfka-king-bucket-v1",
                b"payload-v4",
            )
            path.write_bytes(container)
            parsed = self.info.read_container(path)
            self.assertEqual(parsed["version"], 4)
            self.assertEqual(parsed["perspective_mode"], "dual")
            self.assertEqual(parsed["hidden_shift"], 6)
            self.assertEqual(parsed["output_shift"], 4)
            self.assertTrue(parsed["checksum_valid"])

    def test_detects_payload_checksum_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "corrupt.nnue"
            container = bytearray(_build_container(
                9,
                (7680, 256, 8, 1),
                struct.pack("<BBH", 255, 64, 400),
                b"halfka-hm-v2",
                b"abcdefgh",
            ))
            container[-1] ^= 0xFF
            path.write_bytes(bytes(container))
            parsed = self.info.read_container(path)
            self.assertFalse(parsed["checksum_valid"])

    def test_rejects_truncated_and_trailing_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            valid = _build_container(
                9,
                (7680, 256, 8, 1),
                struct.pack("<BBH", 255, 64, 400),
                b"halfka-hm-v2",
                b"12345678",
            )
            truncated = pathlib.Path(directory) / "truncated.nnue"
            truncated.write_bytes(valid[:-1])
            with self.assertRaises(self.info.NnueInfoError):
                self.info.read_container(truncated)

            trailing = pathlib.Path(directory) / "trailing.nnue"
            trailing.write_bytes(valid + b"\x00")
            with self.assertRaises(self.info.NnueInfoError):
                self.info.read_container(trailing)


if __name__ == "__main__":
    unittest.main()
