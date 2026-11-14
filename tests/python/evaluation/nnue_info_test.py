"""Tests for the NNUE container inspector."""

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


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


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


if __name__ == "__main__":
    unittest.main()
