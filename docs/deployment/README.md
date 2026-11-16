# Deployment

- **[Release images](release-images.md) (human action, open):** a release tag is written once,
  only from an approved, tagged source (`tools/release/publish-release-image.py`); Docker Hub tag
  immutability for `v*` is the image owner's to turn on (lake#994).
- [White-label sign-in](white-label-sign-in.md): sign-in on a Lake that serves a customer's own
  domain, through a Clerk instance for that domain or the customer's IdP alone (lake#1136).
- [Sign-in](sign-in.md): Clerk instances, social-provider setup, production Microsoft ownership,
  legal links, publisher-verification blockers and browser steps, and the terms and privacy links on
  every sign-up and sign-in form (`LAKE_TERMS_URL`, `LAKE_PRIVACY_URL`).
- [Users](users.md): invitations and access removal.
- [Secrets](secrets.md): platform and workspace credential storage.
- [AKS](aks.md): build, roll and deployment checks.
- [How the operator applies a lake version](operator-interface.md): `lake migrate` (the schema
  from the image) and `lake manifest` (Stripe events, products, environment), with their JSON and
  exit codes (lake#1119, orama-operator#936).
- [Client addresses](client-addresses.md): which address per-client limits count, and the
  trusted proxies (`LAKE_TRUSTED_PROXY_CIDRS`).
- [Private instance clone and push](private-install.md): the external origin customers see, and a
  native local instance to clone from and push to.
- [Client addresses](client-addresses.md): which address per-client limits count, the proxy
  listener (`LAKE_PROXY_PORT`) and the trusted proxies (`LAKE_TRUSTED_PROXY_CIDRS`), and the rollout
  order with the operator.
- [Store volume](store-volume.md): the store root on a persistent volume, its lock, and handing it
  from one lake process to the next.

Every setting lake reads is listed in [`../serving/configuration.md`](../serving/configuration.md).
