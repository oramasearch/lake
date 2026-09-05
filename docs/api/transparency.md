# Transparency and TUF metadata

## What

Lake serves the four top-level roles from The Update Framework (TUF) 1.0: `root`, `targets`,
`snapshot`, and `timestamp`. The roles protect repository clients from key compromise, stale
metadata, rollback, and combinations of metadata from different publication states. A signed
checkpoint binds the current root and timestamp metadata. Its configured mode determines whether
Lake may serve it immediately, must first obtain witness cosignatures, or must also retain an
OpenTimestamps-style aggregation proof. Trusted timestamps, tile monitoring, replication, and
agent session keys remain lake #36–#39.

Lake never accepts a private TUF key. Root and targets signing can remain offline, and the online
snapshot and timestamp signers can live in a separate operator-owned process. The API accepts
complete signed metadata, verifies it, and appends it only after the role thresholds and cross-role
bindings pass.

The implementation follows the [TUF specification](https://theupdateframework.github.io/specification/latest/).
The liquid client verifier is tracked in
[`oramasearch/liquid#186`](https://github.com/oramasearch/liquid/issues/186).

## Storage

For server `node_name = <instance>`, immutable JSON objects are stored at:

```text
LAKE_STORE_ROOT/<instance>/log/tuf/<version>.root.json
LAKE_STORE_ROOT/<instance>/log/tuf/<version>.targets.json
LAKE_STORE_ROOT/<instance>/log/tuf/<version>.snapshot.json
LAKE_STORE_ROOT/<instance>/log/tuf/<version>.timestamp.json
```

The instance name must be one safe path component. A write uses a sibling temporary file and an
atomic rename. An existing version is never replaced. Reading without a version selects the
highest stored version for that role; reading with a version returns that immutable object.

Checkpoints are immutable objects beside TUF metadata:

```text
LAKE_STORE_ROOT/<instance>/log/checkpoints/<sequence>.checkpoint.json
LAKE_STORE_ROOT/<instance>/log/checkpoints/<sequence>.ots.json       # anchored only
LAKE_STORE_ROOT/<instance>/log/checkpoints/current.json
```

`current.json` is atomically replaced only after its configured requirements succeed. A checkpoint
contains the exact current root and timestamp metadata, its SHA-256 hash, Lake's TAAP identity
signature, and valid witness signatures. An anchored checkpoint also records the aggregation
endpoint and its opaque proof; the same proof is retained in the adjacent `.ots.json` object.

The API returns both the parsed `metadata` object and `encoded`, the exact compact JSON bytes that
were signed, hashed, and stored. Clients use `encoded` for length and digest checks rather than
serializing the parsed object with their own JSON settings.

## Metadata contract

Every metadata document is the TUF envelope:

```json
{
  "signed": {
    "_type": "root",
    "spec_version": "1.0.36",
    "version": 1,
    "expires": "2099-01-01T00:00:00Z"
  },
  "signatures": [
    {"keyid": "<sha256-of-key-object>", "sig": "<hex-ed25519-signature>"}
  ]
}
```

Lake currently accepts Ed25519 keys and SHA-256 metadata hashes. A key id is the lowercase SHA-256
digest of the compact canonical JSON key object. Each counted signature must have a distinct key
id; repeating one signature cannot satisfy a threshold.

The root must define all four roles, each role's key ids, and a positive threshold. Initial root
metadata is version 1 and meets its own root threshold. A replacement root advances by exactly one
version and meets both the currently trusted root threshold and its own new root threshold.

Targets, snapshot, and timestamp metadata must have future UTC expiry values and versions greater
than the stored versions. Snapshot metadata binds `targets.json` by version, byte length, and
SHA-256 digest. Timestamp metadata binds `snapshot.json` the same way. These checks reject:

- a freeze attempt through expired timestamp metadata;
- a rollback through a non-increasing role version;
- a mixed publication through a mismatched version, length, or hash;
- a compromised or duplicated signer that does not meet the configured threshold.

TUF protects metadata selection. Each targets entry still names the length and digest of a TAAP
attestation object; complete attestation and target verification belongs to the liquid client.

## Runtime configuration and checkpoint protocol

When `LAKE_STORE_ROOT` is configured, Lake reads these runtime values at startup:

| Variable | Required value |
|---|---|
| `LAKE_LOG_MODE` | `air-gapped`, `witnessed` (the default), or `anchored` |
| `LAKE_LOG_WITNESSES` | JSON array of `{endpoint, public_key}` records; `public_key` is a 32-byte Ed25519 hexadecimal public key |
| `LAKE_LOG_WITNESS_THRESHOLD` | Positive integer no greater than the number of distinct witnesses in witnessed or anchored mode |
| `LAKE_LOG_AGGREGATOR` | HTTPS aggregation endpoint, required only in anchored mode |

Endpoints must use HTTPS; loopback HTTP is accepted only for local integration tests. An invalid
mode, an insufficient witness set, duplicate endpoint or key, or anchored mode without an
aggregator makes startup fail with the exact configuration error. Air-gapped mode accepts no
witness or aggregator configuration and never makes an outbound request.

For each checkpoint Lake POSTs this JSON to every configured witness:

```json
{"checkpoint": {"version": "lake_checkpoint/1"}, "checkpoint_hash_sha256": "<hex>"}
```

A witness returns `{"signature":"<hex-ed25519-signature>"}` over the exact compact JSON
checkpoint body. Lake verifies each response against that witness's configured public key and only
publishes `current.json` after the configured threshold of distinct signatures verifies. In
anchored mode, after witness verification Lake POSTs
`{"hash":"<checkpoint SHA-256>","hash_algorithm":"sha256"}` to `LAKE_LOG_AGGREGATOR`; the
aggregator must return `{"proof": ...}`. Lake stores that opaque proof without interpreting it.

Anchoring proves only that the checkpoint hash existed before the aggregation proof's confirmed
chain event. It does not prove authorship, correctness, ownership, truth, an authoritative UTC
time, or agreement on a canonical history. Witnessing is the control against split views; an
anchor supplements it and does not replace its quorum.

## Capabilities

All transparency capabilities use the single registry, require normal authentication, declare the
instance authorization object, and record one signed `transparency` audit event through
`Registry::call` and `audit::record`.

| Capability | Kind | Input | Result |
|---|---|---|---|
| `initialize_tuf` | action | `{root}` | stored root role, version, exact bytes, and parsed metadata |
| `publish_tuf_metadata` | action | `{targets, snapshot, timestamp}` | the root and three appended versions |
| `rotate_tuf_root` | action | `{root}` | the verified successor root |
| `tuf_metadata` | read | `role`, optional positive `version` | latest or named immutable role object |
| `transparency_log_configuration` | read | `{}` | effective mode, public witness set, quorum, and anchored aggregation endpoint when configured |
| `publish_transparency_checkpoint` | action | `{}` | the signed current checkpoint after its mode's requirements complete |
| `transparency_checkpoint` | read | `{}` | current signed checkpoint, or `not_found` before one has met the requirements |

The first three and checkpoint publication require `admin` on the instance transparency log. Reads require `read`. The
operator credential follows the registry's existing explicit administrative bypass; row-resolved
credentials go through `authorize`.

`GET /audit` displays the effective mode and witness quorum through
`transparency_log_configuration`; it does not read configuration outside the registry. Every
configuration read and checkpoint publication records a signed `transparency` audit event.

## Key-compromise recovery

The recovery drill is the production procedure, not a separate emergency code path:

1. Stop metadata publication and identify every exposed key id and role.
2. Generate replacement keys outside lake. Keep the root threshold offline and under distinct
   custodians.
3. Create root version `N+1`. Remove each exposed role key, add its replacement, and set the new
   thresholds. Sign the candidate with the trusted version `N` root threshold and with the
   candidate version `N+1` root threshold.
4. Call `rotate_tuf_root`. Lake refuses a skipped version or either missing threshold.
5. Recreate targets, snapshot, and timestamp metadata with increasing versions, future expiries,
   the replacement role signatures, and freshly computed version/length/hash bindings. Call
   `publish_tuf_metadata`.
6. Fetch every role through `tuf_metadata`, compare the returned exact bytes and versions, verify
   that the removed key can no longer satisfy its role, and retain the signed audit events with
   the incident record.
7. Destroy or revoke the exposed private-key material in its custody system.

If fewer root keys are exposed than the root threshold, the remaining trusted threshold performs
the procedure above. If an attacker controls the complete trusted root threshold, TUF cannot
distinguish the attacker from the repository owners. Recovery then requires distributing a new
trusted root through an authenticated out-of-band channel and explicitly replacing each client's
trust anchor.

`TransparencyTuf.RootRotationReplacesCompromisedRoleKeys` rehearses the procedure. It proves that
old-root-only and new-root-only candidates are refused, a candidate carrying both thresholds is
accepted, removed targets keys are refused, and replacement targets keys publish successfully.

## How to verify

```sh
cmake --build build
./build/tests/lake-tests --gtest_filter='Transparency*'
```

`TransparencyTuf.AppendsThresholdSignedMetadataAndRejectsRepositoryAttacks` covers append, exact
fetch, unique-key thresholds, expired timestamps, rollback, and mixed metadata.
`TransparencyCheckpoint.ValidatesRuntimeConfiguration` covers startup configuration refusal.
`TransparencyCheckpoint.AirGappedSignsAndServesWithoutOutboundCalls` proves air-gapped checkpoint
publication does not invoke its outbound client. The witnessed and anchored test verifies a
threshold of valid Ed25519 witness cosignatures, refuses a checkpoint below that threshold, and
retains an aggregation proof beside the anchored checkpoint. The registry test proves configuration
and checkpoint capability dispatch with signed audit. `PostgresTransparency` repeats the TUF
capabilities with the Postgres control store and verifies the resulting audit chain.

## How to change

Keep private signing keys outside lake. A new key or hash algorithm updates the root validation,
attack fixtures, schema above, and liquid verifier issue together. A new capability belongs in the
single registry and this page. Do not add an unsigned recovery path or overwrite a stored metadata
version.
