# PKI API

## What

Lake creates one private CA per customer. Its root is recovery-capable and its online intermediate
signs short-lived X.509 SPIFFE SVIDs, JWT SVIDs, ACME certificates, SSH certificates, and client
and device certificates. The certificate profiles are credentials for a TAAP workload identity;
they do not create a second identity system.

`create_pki_authority`, `issue_x509_svid`, and `issue_jwt_svid` are registry capabilities. They
authorize against the customer object and record a `certificate` audit event on success, refusal,
or failure. The X.509 response contains a newly generated workload private key; Lake does not
persist that leaf key.

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
that material to the customer key store and registry in the following implementation steps.

## How to verify

The PKI integration suite creates a customer authority, verifies an SVID against its root and
intermediate chain, exercises an ACME order with a test client, and presents an SSH certificate to
a test server. It also confirms issuance and renewal create audit events.

## How to change

Certificate capabilities belong in the registry and authorize against the customer PKI object.
Add a profile to the schema constraint, capability schemas, and this page together. Do not expose
private keys, recovery shares, or protected key-store values through an API response or audit
details.
