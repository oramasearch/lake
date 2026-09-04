# Caller attestation API

## What

Workloads exchange an attestation for a short-lived lake credential bound to their TAAP workload
identity. Supported attestation types are Kubernetes projected service-account tokens through a
configured TokenReview endpoint, OIDC/JWT federation, AWS instance identity, and Azure instance
identity. No workload bootstrap secret is required.

Federated issuers use the `federation_issuers` policy table shared with organization SAML/OIDC
configuration. Production caller exchange performs an exact store lookup for every asserted claim;
an injected policy implementation remains available to isolated verifier tests.

The attestation boundary has four injected verifiers: Kubernetes TokenReview, JWT/OIDC signature
verification, AWS instance identity, and Azure instance identity. A verified result is insufficient
on its own: JWT claims must each be allowed by the organization issuer policy before Lake mints a
credential. This keeps trust-graph policy distinct from transport-specific verification.

`ServerOptions` receives those configured dependencies and supplies one caller-attestation service
to the registry. `exchange_kubernetes_attestation`, `exchange_jwt_attestation`, and
`exchange_cloud_attestation` are public-entry capabilities; successful exchanges are audit events.
Each public entry still declares the customer object and policy action required by the registry
contract; attestation verification supplies authentication instead of a pre-existing bearer.
Before returning the bearer credential, each exchange writes a `pki_callers` row containing its
customer, workload identity, attestation type, verified issuer and subject, non-secret verified
attributes, and expiry. The credential value is never persisted. The response includes the caller
record ID so an operator can correlate the exchange with its signed audit event.

## How to verify

The caller suite exchanges a Kubernetes token against a TokenReview double, rejects an
unregistered issuer claim, accepts a configured federation claim, and verifies the minted
credential expires. Every result is present in the signed audit stream.
The Postgres-backed test repeats Kubernetes and JWT exchanges, resolves the JWT claim through a
durable federation issuer, and reads both durable caller records.

## How to change

Keep issuer policy resolution behind the caller-policy interface. Do not add an alternative
authentication route in an HTTP handler; capability dispatch and `authorize` remain the only
control-plane authorization path.
