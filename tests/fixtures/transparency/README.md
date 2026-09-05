# Transparency conformance fixture

`fixture.json` is the deterministic wire-format fixture for Lake entry tiles, parent-hash tiles,
checkpoint binding, inclusion proofs, and consistency proofs. Its seven UTF-8 entries make a
non-perfect tree, so verification covers both balanced subtrees and the shorter right edge. The
checkpoint carries deterministic Ed25519 Lake and witness signatures; `lake_identity` supplies the
Lake public key for consumers that also verify the checkpoint signature.

Run `./tests/fixtures/transparency/regenerate.py` to replace the fixture or add `--check` to verify
that it is current. The generator requires Python 3 and the OpenSSL executable already required by
the Lake build. It uses fixed private seeds only for this public test vector; those keys are not
credentials and must never be used outside the fixture.

The executable test decodes the fixture without calling Lake's Merkle implementation. It recomputes
the checkpoint digest, verifies both witness signatures, rebuilds the root and every parent-hash
tile, verifies inclusion for all seven leaves, verifies the size-3 to size-7 consistency path, and
requires changed entry-tile, hash-tile, inclusion-path, and consistency-path bytes to fail
verification.
