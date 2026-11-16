# Signed Audit and Export Manifests

`audit.cpp` signs Lake's audit events and portable export manifests. Its instance identities
derive their TAAP name from the SHA-512/256 digest of the Ed25519 public key in uppercase,
unpadded base32. `verify_export_manifest` requires that exact binding before checking the
manifest signature (lake#954); callers cannot substitute another instance's name.

Self-contained verification establishes key possession and archive consistency. Import handles
source trust separately, treating supplied metadata as untrusted. The format, grant attribution,
and regression coverage are in [export.md](../../docs/api/export.md).
