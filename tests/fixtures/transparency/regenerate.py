#!/usr/bin/env python3

import argparse
import base64
import hashlib
import json
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parent
FIXTURE = ROOT / "fixture.json"
ENTRY_HEADER = b"LAKE-TILE-1\n"
HASH_HEADER = b"LAKE-HASH-TILE-1\n"
TILE_WIDTH = 1024


def sha256(value):
    return hashlib.sha256(value).digest()


def leaf_hash(value):
    return sha256(b"\x00" + value)


def node_hash(left, right):
    return sha256(b"\x01" + left + right)


def base64url(value):
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def encode_entry_tile(entries):
    output = bytearray(ENTRY_HEADER)
    for entry in entries:
        output.extend(len(entry).to_bytes(4, "big"))
        output.extend(entry)
    return bytes(output)


def encode_hash_tile(hashes):
    return HASH_HEADER + b"".join(hashes)


def largest_power_of_two_less_than(value):
    return 1 << ((value - 1).bit_length() - 1)


def merkle_root(entries):
    if not entries:
        return sha256(b"")
    if len(entries) == 1:
        return leaf_hash(entries[0])
    split = largest_power_of_two_less_than(len(entries))
    return node_hash(merkle_root(entries[:split]), merkle_root(entries[split:]))


def inclusion_path(entries, leaf_index):
    if len(entries) == 1:
        return []
    split = largest_power_of_two_less_than(len(entries))
    if leaf_index < split:
        return inclusion_path(entries[:split], leaf_index) + [merkle_root(entries[split:])]
    return inclusion_path(entries[split:], leaf_index - split) + [merkle_root(entries[:split])]


def consistency_path(entries, old_size, complete=True):
    new_size = len(entries)
    if old_size == new_size:
        return [] if complete else [merkle_root(entries)]
    split = largest_power_of_two_less_than(new_size)
    if old_size <= split:
        return consistency_path(entries[:split], old_size, complete) + [
            merkle_root(entries[split:])
        ]
    return consistency_path(entries[split:], old_size - split, False) + [
        merkle_root(entries[:split])
    ]


def cbor_length(major, value):
    prefix = major << 5
    if value < 24:
        return bytes([prefix | value])
    if value <= 0xFF:
        return bytes([prefix | 24, value])
    if value <= 0xFFFF:
        return bytes([prefix | 25]) + value.to_bytes(2, "big")
    if value <= 0xFFFFFFFF:
        return bytes([prefix | 26]) + value.to_bytes(4, "big")
    return bytes([prefix | 27]) + value.to_bytes(8, "big")


def cbor(value):
    if value is None:
        return b"\xf6"
    if value is False:
        return b"\xf4"
    if value is True:
        return b"\xf5"
    if isinstance(value, int):
        return cbor_length(0, value)
    if isinstance(value, str):
        encoded = value.encode("utf-8")
        return cbor_length(3, len(encoded)) + encoded
    if isinstance(value, list):
        return cbor_length(4, len(value)) + b"".join(cbor(item) for item in value)
    if isinstance(value, dict):
        items = sorted(value.items())
        return cbor_length(5, len(items)) + b"".join(
            cbor(key) + cbor(item) for key, item in items
        )
    raise TypeError(f"unsupported CBOR value: {type(value).__name__}")


def ed25519(seed, message, directory, name):
    key_path = directory / f"{name}.der"
    message_path = directory / f"{name}.message"
    signature_path = directory / f"{name}.signature"
    key_path.write_bytes(bytes.fromhex("302e020100300506032b657004220420") + seed)
    message_path.write_bytes(message)
    public_der = subprocess.run(
        [
            "openssl",
            "pkey",
            "-inform",
            "DER",
            "-in",
            str(key_path),
            "-pubout",
            "-outform",
            "DER",
        ],
        check=True,
        capture_output=True,
    ).stdout
    subprocess.run(
        [
            "openssl",
            "pkeyutl",
            "-sign",
            "-rawin",
            "-inkey",
            str(key_path),
            "-keyform",
            "DER",
            "-in",
            str(message_path),
            "-out",
            str(signature_path),
        ],
        check=True,
        capture_output=True,
    )
    return public_der[-32:], signature_path.read_bytes()


def tile_value(kind, level, encoded, width):
    value = {
        "encoding": "base64url",
        "index": 0,
        "kind": kind,
        "level": level,
        "sha256": hashlib.sha256(encoded).hexdigest(),
        "tile": base64url(encoded),
        "version": "lake_tile/1" if level == 0 else "lake_hash_tile/1",
    }
    value["entries" if level == 0 else "hashes"] = width
    return value


def make_fixture():
    entries = [f"lake transparency fixture entry {index}".encode("utf-8") for index in range(7)]
    root_hash = merkle_root(entries).hex()
    levels = []
    current = [leaf_hash(entry) for entry in entries]
    while len(current) >= 2:
        current = [
            node_hash(current[index], current[index + 1])
            for index in range(0, len(current) - 1, 2)
        ]
        if current:
            levels.append(current)
    checkpoint = {
        "hash_algorithm": "sha256",
        "hash_tile_width": TILE_WIDTH,
        "root": {"signed": {"_type": "root", "version": 1}, "signatures": []},
        "root_hash": root_hash,
        "root_version": 1,
        "tile_width": TILE_WIDTH,
        "timestamp": {"signed": {"_type": "timestamp", "version": 1}, "signatures": []},
        "timestamp_version": 1,
        "tree_size": len(entries),
        "version": "lake_checkpoint/1",
    }
    checkpoint_encoded = json.dumps(checkpoint, sort_keys=True, separators=(",", ":")).encode()
    with tempfile.TemporaryDirectory(prefix="keys-", dir=ROOT) as temporary:
        directory = pathlib.Path(temporary)
        lake_public, lake_signature = ed25519(
            bytes(range(1, 33)),
            b"TAAP/1 lake_checkpoint/1\x00" + cbor(checkpoint),
            directory,
            "lake",
        )
        witnesses = []
        for index, first in enumerate((65, 129), start=1):
            public_key, signature = ed25519(
                bytes((first + offset) % 256 for offset in range(32)),
                checkpoint_encoded,
                directory,
                f"witness-{index}",
            )
            witnesses.append(
                {
                    "endpoint": f"https://witness-{index}.example/checkpoint",
                    "public_key": public_key.hex(),
                    "signature": signature.hex(),
                }
            )
    checkpoint_hash = hashlib.sha256(checkpoint_encoded).hexdigest()
    checkpoint_record = {
        "checkpoint": checkpoint,
        "checkpoint_hash_sha256": checkpoint_hash,
        "sequence": 1,
        "signature": {
            "algorithm": "ed25519",
            "signature": base64url(lake_signature),
            "signer": "taap:1:ed25519:fixture-lake",
        },
        "witness_signatures": witnesses,
    }
    inclusion_proofs = []
    for leaf_index in range(len(entries)):
        inclusion_proofs.append(
            {
                "audit_path": [item.hex() for item in inclusion_path(entries, leaf_index)],
                "checkpoint_hash_sha256": checkpoint_hash,
                "checkpoint_sequence": 1,
                "hash_algorithm": "sha256",
                "leaf_index": leaf_index,
                "root_hash": root_hash,
                "tree_size": len(entries),
                "version": "lake_inclusion_proof/1",
            }
        )
    old_size = 3
    consistency_proof = {
        "audit_path": [item.hex() for item in consistency_path(entries, old_size)],
        "checkpoint_hash_sha256": checkpoint_hash,
        "checkpoint_sequence": 1,
        "hash_algorithm": "sha256",
        "new_root_hash": root_hash,
        "new_tree_size": len(entries),
        "old_root_hash": merkle_root(entries[:old_size]).hex(),
        "old_tree_size": old_size,
        "version": "lake_consistency_proof/1",
    }
    tiles = [tile_value("entries", 0, encode_entry_tile(entries), len(entries))]
    for level, hashes in enumerate(levels, start=1):
        tiles.append(tile_value("hashes", level, encode_hash_tile(hashes), len(hashes)))
    return {
        "checkpoint": checkpoint_record,
        "entries": [base64url(entry) for entry in entries],
        "lake_identity": {
            "public_key": base64url(lake_public),
            "taap_id": "taap:1:ed25519:fixture-lake",
            "version": "taap_identity/1",
        },
        "proofs": {
            "consistency": consistency_proof,
            "inclusion": inclusion_proofs,
        },
        "tiles": tiles,
        "version": "lake_transparency_fixture/1",
    }


def encoded_fixture():
    return json.dumps(make_fixture(), indent=2, sort_keys=True) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--stdout", action="store_true")
    args = parser.parse_args()
    encoded = encoded_fixture()
    if args.stdout:
        print(encoded, end="")
        return
    if args.check:
        if not FIXTURE.exists() or FIXTURE.read_text() != encoded:
            raise SystemExit("fixture.json is stale; run regenerate.py")
        return
    FIXTURE.write_text(encoded)


if __name__ == "__main__":
    main()
