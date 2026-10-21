# Test fixtures

`schema-tuf.json` contains a complete signed TUF root, targets, snapshot, and timestamp set for the
HTTP schema conformance test. Each role has one Ed25519 signer and threshold one. The metadata
expires in 2099; snapshot and timestamp bind the canonical JSON length and SHA-256 digest of the
preceding role. No private key is retained. This fixture exercises the production TUF verifier
before the test publishes a real signed checkpoint and requests proof tools over MCP and REST.
Replace the complete set with freshly signed, mutually bound documents when changing it; editing
a signed field directly must fail verification. The conformance test is the verification command.

`transparency/` holds the independent byte-level transparency verifier fixtures; see its
[`README.md`](transparency/README.md).

`saml/` holds SAML responses for `tests/test_saml.cpp` (lake#836), signed by signxml (lxml and
libxml2), an XML Signature implementation independent of Lake's: one shaped like Okta's (response and
assertion both signed, an `InclusiveNamespaces` prefix list) and one like Microsoft Entra's (only the
assertion signed, default namespaces), with the certificate that verifies them. They are valid at
2026-10-21T12:01Z, which the tests pass as the time. `make-fixtures.py` rewrites all three with a new
key (`uv run --no-project --with signxml python -I tests/fixtures/saml/make-fixtures.py`); it keeps no
private key, and it is not part of the build.
