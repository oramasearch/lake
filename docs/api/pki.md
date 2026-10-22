# PKI API

## What

Lake creates one private CA per customer. Its root is recovery-capable and its online intermediate
signs short-lived X.509 SPIFFE SVIDs, JWT SVIDs, ACME certificates, SSH certificates, and client
and device certificates. The certificate profiles are credentials for a TAAP workload identity;
they do not create a second identity system.

`create_pki_authority`, `issue_x509_svid`, and `issue_jwt_svid` are registry capabilities. They
authorize against the customer object and record a `certificate` audit event. The X.509 response
contains a newly generated workload private key; Lake does not persist that leaf key.

`issue_client_certificate` and `issue_device_certificate` issue the same short-lived chain for a
named client or device subject.

`issue_agent_certificate` accepts the 32-byte base64url Ed25519 public key from an agent's TAAP
identity and issues a certificate with that TAAP identifier as its URI subject alternative name.
The private key never enters Lake. `rotate_agent_certificate` issues a new certificate row linked
by `renewed_from_id`; it does not silently change the old row. `revoke_agent_certificate` records
immediate revocation, and `check_agent_certificate` rejects a revoked or expired certificate at
the API boundary. Lifetimes are positive and capped at one hour.

### Certificate identities

Lake encodes each subject alternative name directly as one typed ASN.1 `GENERAL_NAME`
(lake#946, LSR-084). Caller text never enters OpenSSL's extension configuration parser. X.509
SVIDs and agent certificates contain exactly one URI SAN; ACME certificates contain exactly one
DNS SAN. Tunnel certificates use the same encoder for their grant URI, with the existing
recipient-only authorization. Generic client and device subjects remain common names only.

ACME identifiers must be ASCII LDH hostnames: at most 253 bytes, with at least two nonempty
labels of at most 63 letters, digits or hyphens, each beginning and ending with a letter or
digit. Wildcards, trailing dots, underscores, whitespace, URI punctuation and embedded NULs
answer `invalid_input`. ASCII Punycode labels are accepted. The order and final signing path
both enforce this rule.

X.509 SPIFFE identities follow the [SPIFFE ID syntax](https://github.com/spiffe/spiffe/blob/main/standards/SPIFFE-ID.md):
`spiffe://`, a nonempty lowercase trust domain of at most 255 bytes using `[a-z0-9._-]`, and an
optional path with nonempty `[a-zA-Z0-9._-]` segments. Lake caps the identity at 2048 bytes and
rejects relative segments, trailing slashes, ports, user information, queries, fragments and
percent encoding. URI SANs in all profiles reject commas, whitespace, non-ASCII bytes and NULs.
These stricter input rules deliberately reject malformed identities previously accepted.

## What the exit scenario proves

The identity-exit Postgres scenario uses the same libliquid actor that signed in, gives Lake only
its public TAAP key, checks the first short-lived certificate, rotates it, revokes the first
certificate, observes its API check fail with `unauthorized`, and confirms the replacement remains
valid. Issue, check, rotation, revocation, denied old-certificate check, and replacement check are
retained as signed `certificate` audit events.

`issue_ssh_user_certificate` and `issue_ssh_host_certificate` accept an Ed25519 OpenSSH public
key, a key ID, one or more principals, and an optional lifetime. Lake returns a native OpenSSH
certificate plus the customer CA public key. The online P-256 intermediate signs both profiles;
the root stays offline, the caller's private key never enters Lake, and lifetimes cannot exceed
one hour.

## ACME

Each customer has an RFC 8555 directory at `GET /acme/<customer-id>/directory`. It advertises
`newNonce`, `newAccount`, and `newOrder`. Account and order requests use flattened JWS JSON with
ES256, a one-use `Replay-Nonce`, and either the account P-256 JWK or the account URL in `kid`.
The ACME registry transport is not rendered as generic `/v1` routes or MCP tools, so those paths
cannot bypass JWS verification. Its public JWS entry points still declare their customer object
and policy action in registry metadata; HTTP-01 token reads declare the challenge object.

Lake implements the order flow used by cert-manager's HTTP-01 solver: new account, one DNS
identifier per order, authorization lookup by POST-as-GET, an `http-01` challenge, finalize with a
PKCS #10 DER CSR, order polling, and `application/pem-certificate-chain` download. During the
challenge Lake serves the account-key authorization at
`GET /.well-known/acme-challenge/<token>`. Finalization verifies the CSR signature and ordered DNS
SAN, then signs that CSR public key with the customer's online intermediate. Nonces are consumed
once, and every state transition and challenge access calls an ACME registry capability and
records an audit event.

### Bounded ACME State

Every ACME route answers without a credential, so nothing a client sends grows Lake's memory
without limit (lake#940). The bounds are `pki::AcmeLimits`; the defaults are below.

- A `Replay-Nonce` keeps no state when it is issued: it is a counter and the issue time under an
  HMAC-SHA-256 with a key each process draws at start. It is accepted once, within five minutes,
  by the process that issued it and in exactly the encoding it was sent. Only consumed nonces are
  remembered, at most 16,384; past that the oldest is forgotten and every nonce issued before it
  answers `badNonce`, so a forgotten nonce is never accepted again. A client that gets `badNonce`
  retries with the nonce on that answer, as RFC 8555 expects.
- `newAccount` needs a customer that exists (404 otherwise). A customer keeps at most 16 accounts
  and the process 4,096; past either cap the least recently used account goes, with its orders. An
  account unused for 24 hours goes too.
- An order and its authorization go one hour after they were created, and an account keeps at
  most 8 orders, losing its oldest past that. An HTTP-01 token is looked up in an index, not by
  scanning every authorization.

Accounts and orders live in the process's memory, as before: a restart forgets them, and the
client registers again.

The API server owns the PKI service for its process and supplies it to every registry call. The
service creates and loads customer authority records through the control-plane store; callers must
not substitute an HTTP-local signer for that record.

Private CA material is envelope-encrypted through the control-plane key store. The root is kept
out of ordinary issuance and the online intermediate signs leaf credentials. Customer-KEK rotation
rewraps PKI private-key ciphertext. X.509 profiles default to one hour; JWT SVIDs default to
fifteen minutes. A caller requesting a longer lifetime must supply a justification, which is
retained in the signed audit event.

The cryptographic authority builder creates a P-256 root and intermediate, verifies each X.509
SVID through that chain, and signs compact ES256 JWT SVIDs. Persistence and API dispatch bind
that material to the customer key store and registry.

## How to verify

The PKI integration suite creates a customer authority, verifies an SVID against its root and
intermediate chain, and parses issued user and host certificates with `ssh-keygen -L`. It also
runs a minimal ES256 ACME client against a live lake server. The client
gets a nonce, creates an account and order, reads Lake's HTTP-01 response, answers the challenge,
submits a signed CSR, polls the order, and downloads the certificate chain. The suite also
confirms issuance and renewal create audit events. `Pki.AcmeNoncesAreStatelessOneUseAndExpire`,
`Pki.AcmeAccountsAndOrdersAreBoundedAndOrdersExpire` and
`Pki.AcmeNewAccountForAnUnknownCustomerIsRefused` hold the bounds above on a test clock.
`Pki.X509SvidRejectsMalformedSpiffeIdentities`,
`Pki.AgentCertificateRejectsSanConfigurationDelimiters` and
`Pki.AcmeRejectsMalformedDnsIdentifiers` reject extra SAN entries and malformed names; the SVID,
agent and HTTP ACME tests decode the issued certificate and assert its single SAN's type and
exact bytes. Run these with `ctest --test-dir build -R 'Pki\.' --timeout 600 --output-on-failure`.

## How to change

Certificate capabilities belong in the registry and authorize against the customer PKI object.
Add a profile to the schema constraint, capability schemas, and this page together. Do not expose
private keys, recovery shares, or protected key-store values through an API response or audit
details.
Keep identity validation at order creation and the shared certificate encoder. Add SANs through
typed `GENERAL_NAME` values and `X509_add1_ext_i2d`; configuration strings are only for fixed
extension constants.
