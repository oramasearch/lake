# Caller attestation API

## What

Workloads exchange an attestation for a short-lived lake credential bound to their TAAP workload
identity. Supported attestation types are Kubernetes projected service-account tokens through a
configured TokenReview endpoint, OIDC/JWT federation, AWS instance identity, and Azure instance
identity. No workload bootstrap secret is required.

Federated issuers use the `federation_issuers` policy table introduced by the SSO branch. Until
that branch merges, this work exposes a narrow issuer-policy interface with an in-memory stand-in;
the rebase replaces it with the SSO store implementation without changing the attestation API.

The attestation boundary has four injected verifiers: Kubernetes TokenReview, JWT/OIDC signature
verification, AWS instance identity, and Azure instance identity. A verified result is insufficient
on its own: JWT claims must each be allowed by the organization issuer policy before Lake mints a
credential. This keeps trust-graph policy distinct from transport-specific verification.

## How to verify

The caller suite exchanges a Kubernetes token against a TokenReview double, rejects an
unregistered issuer claim, accepts a configured federation claim, and verifies the minted
credential expires. Every result is present in the signed audit stream.

## How to change

Keep issuer policy resolution behind the caller-policy interface. Do not add an alternative
authentication route in an HTTP handler; capability dispatch and `authorize` remain the only
control-plane authorization path.
