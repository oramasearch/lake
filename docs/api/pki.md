# PKI API

## What

Lake creates one private CA per customer. Its root is recovery-capable and its online intermediate
signs short-lived X.509 SPIFFE SVIDs, JWT SVIDs, ACME certificates, SSH certificates, and client
and device certificates. The certificate profiles are credentials for a TAAP workload identity;
they do not create a second identity system.

Private CA material is envelope-encrypted through the control-plane key store. The root is kept
out of ordinary issuance and the online intermediate signs leaf credentials. X.509 profiles
default to one hour; JWT SVIDs default to fifteen minutes. A caller requesting a longer lifetime
must supply a justification, which is retained in the signed audit event.

## How to verify

The PKI integration suite creates a customer authority, verifies an SVID against its root and
intermediate chain, exercises an ACME order with a test client, and presents an SSH certificate to
a test server. It also confirms issuance and renewal create audit events.

## How to change

Certificate capabilities belong in the registry and authorize against the customer PKI object.
Add a profile to the schema constraint, capability schemas, and this page together. Do not expose
private keys, recovery shares, or protected key-store values through an API response or audit
details.
