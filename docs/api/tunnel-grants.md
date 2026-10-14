# Developer tunnel credentials

Lake [#730](https://github.com/oramasearch/lake/issues/730) adds recipient-only client
certificate delivery to the [network capabilities](network.md), implementing Lake's part of
the operator's secure-networking contract, section 11.1. This does not claim a deployed relay or
end-to-end session termination exercise.

## Grant, obtain credentials, connect, revoke

An account admin calls `grant_network_tunnel` with `customer_id`, `workspace_id`, the recipient's
persisted `user_id`, `target_kind` (`service` or `app`), `target_id`, `port` and future UTC
`expires_at`. A served deployment uses its service target. The recipient must be able to read the
account. A write-only member cannot grant or revoke. Lake loads or creates the customer's existing
PKI authority and pins its public root in `client_ca_pem` on the new grant. No private CA material
is written to a network row or returned by these capabilities.
The instance must have its existing PKI/instance-KEK configuration; missing signing capability or
unreadable protected CA keys fail closed.

After the operator reports `status.phase: Ready`, an `observed_generation` matching the desired
generation, a nonempty relay `endpoint` and `relay_ca_pem`, the recipient calls
`POST /v1/issue_network_tunnel_certificate` (or the MCP tool with that name):

```json
{
  "customer_id": "<customer UUID>",
  "workspace_id": "<workspace UUID>",
  "tunnel_grant_id": "<grant UUID>",
  "csr_pem": "-----BEGIN CERTIFICATE REQUEST-----\n..."
}
```

The caller must have account read permission, a persisted user matching the authenticated TAAP
identity, and `caller.user_id == grant.user_id`. Neither another admin nor the operator credential
can request the recipient's certificate. An admin who is also the recipient can request their own.
A revoked or expired grant, stale/incomplete relay observation, or missing/mismatched client CA
fails closed. The handler rereads the grant after signing and refuses changes observed during
issuance. The relay still checks current grants on connection and during live sessions.
The grant URI namespace is reserved across the customer's other PKI profiles too: generic agent,
SPIFFE and DNS issuance cannot produce that SAN, including through an injected extra SAN. This
prevents an admin from using another shared-CA signing route to bypass recipient-only delivery.

The success value has exactly `certificate_pem`, `chain_pem`, `expires_at`, `endpoint` and
`relay_ca_pem`. `chain_pem` contains the intermediate and root. The certificate uses the CSR's
verified public key, one URI SAN `urn:orama:tunnel-grant:<grant UUID>`, CA=false and clientAuth only.
Requested subject names, SANs, CA privileges and extensions are not copied. Only a single valid
PEM PKCS#10 request up to 16 KiB is accepted; a valid signature and at least 112 security bits are
required. Validity ends no later than the grant's expiry, issuance plus twelve hours, or either
issuing CA's expiry. Whole-second certificate precision is rounded down, never up.

Generate and retain the key locally, for example:

```sh
umask 077
openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
  -keyout tunnel.key -out tunnel.csr -subj '/CN=developer'
```

Send only `tunnel.csr` as `csr_pem`. Save `certificate_pem` followed by `chain_pem` in
`tunnel.crt`, and save `relay_ca_pem` in `relay-ca.crt`. With the returned endpoint:

```sh
zetta-tunnel client --cert tunnel.crt --key tunnel.key --ca relay-ca.crt --relay '<endpoint>'
```

The private key never touches Lake. While the grant remains active, its recipient can submit
another CSR for another bounded certificate. Existing grants without a pinned CA are not silently
upgraded: revoke/regrant after migration and wait for operator readiness.

`revoke_network_tunnel` sets `revoked_at` and increments `generation` in an audited transaction.
The operator removes that grant from the relay's grants file; zetta-tunnel ends its live sessions.
Expiry also makes the grant inactive without depending on a status refresh. Setting a database
field alone does not prove that a live socket has already disconnected.

## Audit and session history

The signed control-plane audit records grant/revoke actor, request, object, workspace, generation
and time. Issuance uses the central certificate audit and records recipient, grant, target kind/ID,
port, generation, certificate expiry and SHA-256 of the returned certificate PEM. It does not log CSR, key or certificate
PEM. Refused capability calls also receive the ordinary denied/failure outcome audit.
MCP marks issuance as a writing, non-idempotent, non-destructive action: the read permission
requirement does not turn signing a fresh credential into a read-only operation.

`NetworkRecords` is a scoped asynchronous provider for session history and network activity.
`ServerOptions.network_records` injects it; null selects `PendingNetworkRecords`. There is no
invented Zetta table or event schema. The default replies are successful, explicit placeholders:

```json
{"sessions": [], "available": false, "pending": "zetta#1281"}
```

`network_activity` uses `events` instead of `sessions`. Both require account read access before
calling the provider; `limit` defaults to 50 and is bounded to 1–100. Available providers set
`available: true`, `pending: ""`, and return at most that many opaque record objects. Malformed
provider results fail rather than pretending to be empty history. Real live/past sessions and
who/what/when session records remain **pending zetta#1281**. The coordinator owns the relay
grant/connect/revoke/session-ends end-to-end proof.

## Verification and changes

`TunnelCredentials` runs against memory and PostgreSQL and covers CSR signing and rejected
requests, validity bounds, recipient-only authorization, admin-only mutations, revocation,
expiry, trust pinning and signed audit. `NetworkCapabilities` exercises provider authorization,
scope, bounds and the pending default. `PostgresNetwork` covers migration columns/defaults,
existing rows and replay. `SchemaConformance` exercises actual REST and MCP transports and
independently validates required output fields. See [the proof record](../build/tunnel-grants-proof.md).

Changes to these fields require updating this page, registry schemas and transport evidence.
The two migration columns follow section 11.1; do not rewrite `0084_network`. Adopt zetta#1281's
real contract before implementing a production history reader.
