"""Print Koi NNUE container provenance without loading an engine.

Usage:
    python tools/measurement/nnue_info.py artifacts/networks/koi-v9-hm10-h256-official.nnue [--json] [--verify]

Supports the v4/v5/v6/v8/v9 container layouts (magic, version, four layer sizes,
quantization/scale metadata, quantization/feature-set strings, payload SHA-256
hash, size, and checksum verification). Older v2/v3 layouts print the magic,
version, and raw header length.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct
import sys

MAGIC = b"KOI-NNUE"
HEADER = struct.Struct("<8sIIIIIBBBBHHQ")
SCALE_U8_U8_U16 = struct.Struct("<BBH")
SUPPORTED_LAYOUTS = {4, 5, 6, 8, 9}


class NnueInfoError(RuntimeError):
    """Raised for unusable containers."""


def read_container(path: pathlib.Path) -> dict[str, object]:
    data = path.read_bytes()
    if len(data) < HEADER.size or data[:8] != MAGIC:
        raise NnueInfoError("not a Koi NNUE container")
    (magic, version, input_units, hidden, buckets, l1,
     byte28, byte29, byte30, byte31,
     quantization_size, feature_set_size, payload_size) = HEADER.unpack_from(data, 0)
    info: dict[str, object] = {
        "path": str(path),
        "size_bytes": len(data),
        "file_sha256": hashlib.sha256(data).hexdigest(),
        "magic": magic.decode(),
        "version": version,
        "input_units": input_units,
        "hidden": hidden,
        "output_buckets": buckets,
        "l1": l1,
        "quantization_size": quantization_size,
        "feature_set_size": feature_set_size,
        "payload_size": payload_size,
        "header_bytes": HEADER.size,
    }
    if version == 9:
        qa, qb, eval_scale = SCALE_U8_U8_U16.unpack_from(data, 28)
        info["qa"] = qa
        info["qb"] = qb
        info["eval_scale"] = eval_scale
    elif version == 4:
        info["hidden_shift"] = byte28
        info["output_shift"] = byte29
        info["perspective_mode"] = "dual" if l1 == 2 else "single"
    elif version in {5, 6, 8}:
        info["hidden_shift"] = byte28
        info["output_shift"] = byte29
        info["l1_shift"] = byte30
        info["l1_diff_shift"] = byte31
    else:
        info["layout"] = "unsupported-for-detail"
        return info

    offset = HEADER.size
    if len(data) - offset < 32:
        raise NnueInfoError("container is truncated before the payload hash")
    info["network_sha256"] = data[offset:offset + 32].hex()
    offset += 32
    if len(data) - offset < quantization_size + feature_set_size:
        raise NnueInfoError("container is truncated inside header strings")
    quantization = data[offset:offset + quantization_size].decode("ascii", "replace")
    offset += quantization_size
    feature_set = data[offset:offset + feature_set_size].decode("ascii", "replace")
    offset += feature_set_size
    payload = data[offset:offset + payload_size]
    if len(payload) != payload_size:
        raise NnueInfoError("container is truncated inside the payload")
    if offset + payload_size != len(data):
        raise NnueInfoError("container has unexpected trailing bytes after payload")
    payload_sha256 = hashlib.sha256(payload).hexdigest()
    info["quantization"] = quantization
    info["feature_set"] = feature_set
    info["payload_sha256"] = payload_sha256
    info["checksum_valid"] = payload_sha256 == info["network_sha256"]
    return info


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("container", type=pathlib.Path)
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    parser.add_argument(
        "--verify",
        action="store_true",
        help="exit nonzero unless the embedded payload SHA-256 checksum is valid",
    )
    arguments = parser.parse_args(argv)
    try:
        info = read_container(arguments.container)
    except (OSError, NnueInfoError) as error:
        print(f"nnue_info: {error}", file=sys.stderr)
        return 1
    if arguments.json:
        print(json.dumps(info, indent=2))
    else:
        for key, value in info.items():
            print(f"{key}: {value}")
    if arguments.verify and info.get("checksum_valid") is not True:
        print("nnue_info: payload SHA-256 checksum verification failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
