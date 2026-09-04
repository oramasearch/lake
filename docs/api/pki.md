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
cannot bypass JWS verification.

Lake implements the order flow used by cert-manager's HTTP-01 solver: new account, one DNS
identifier per order, authorization lookup by POST-as-GET, an `http-01` challenge, finalize with a
PKCS #10 DER CSR, order polling, and `application/pem-certificate-chain` download. During the
challenge Lake serves the account-key authorization at
`GET /.well-known/acme-challenge/<token>`. Finalization verifies the CSR signature and ordered DNS
SAN, then signs that CSR public key with the customer's online intermediate. Nonces are consumed
once, and every state transition and challenge access calls an ACME registry capability and
records an audit event.

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
confirms issuance and renewal create audit events.

## How to change

Certificate capabilities belong in the registry and authorize against the customer PKI object.
Add a profile to the schema constraint, capability schemas, and this page together. Do not expose
private keys, recovery shares, or protected key-store values through an API response or audit
details.
