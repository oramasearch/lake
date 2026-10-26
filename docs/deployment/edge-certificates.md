# Runbook: edge certificates for app hosts, and the zone settings (human action)

> **Human action: done (2026-10-26).** The Cloudflare steps below were taken on 2026-10-26, and the edge certificates are
> active on both zones ([lake#798](https://github.com/oramasearch/lake/issues/798), closed). Keep this page for a new zone or a
> replaced token: the same steps apply.

## Symptom

- An app or preview host fails the TLS handshake at Cloudflare (`ssl/tls alert handshake failure`, alert 40), for example
  `https://<app>.lake-dev.oramadev.net` or `https://<app>.eu.lake.live`. The Lake's own host works.
- On `lake.live`: authenticated origin pulls, Full (strict) and Always Use HTTPS are not applied.

## Why

App and preview hosts are two levels below the zone (`<app>.eu.lake.live`). Cloudflare's free Universal SSL covers only
the zone and one level below it (`lake.live`, `*.lake.live`). The operator's `EdgeZonePolicy` orders an **Advanced
Certificate** for each apps wildcard ([orama-operator#745](https://github.com/oramasearch/orama-operator/pull/745), in every release since v0.8.0) and applies the zone's TLS
settings, but only when the zone has Advanced Certificate Manager and the token may do it. No operator code or resource
is missing. Design: orama-operator
[`docs/lake/origin-tls.md`](https://github.com/oramasearch/orama-operator/blob/dev/docs/lake/origin-tls.md#the-tokens-permissions).

## Check the current state (read-only, anyone)

```sh
# nonprod (KUBECONFIG = the nonprod leaf's kubeconfig, from Secret orama-system/orama-nonprod-cluster-01-kubeconfig)
kubectl get edgezonepolicies -A
kubectl get edgezonepolicies -A -o jsonpath='{range .items[*]}{.metadata.name}{"\n"}{range .status.conditions[*]}  {.type}={.status} {.reason}: {.message}{"\n"}{end}{end}'
# production: the same against orama-production-cluster-02 and -03
```

What the conditions mean:

| Condition and reason | Missing |
|---|---|
| `Credentials False ZoneSettingsUnreadable` | the token's **Zone > Zone Settings** permission |
| `EdgeCertificateOrders False OrderRefused` naming *SSL and Certificates* | the token's **Zone > SSL and Certificates > Edit** |
| `EdgeCertificateOrders False` naming *Advanced Certificate Manager* | the zone's ACM subscription |
| `EdgeCertificates True`, `EdgeCertificateOrders True` | nothing: done |

State on 2026-10-26, after the steps: both zones `Credentials True`; Advanced Certificates active for `*.eu.lake.live`,
`*.us.lake.live`, `*.lake-dev.oramadev.net`, `*.lake-testing.oramadev.net` and `*.lake-staging.oramadev.net`.

## Steps (Steve, in the Cloudflare dashboard)

The tokens are **account** API tokens (*Manage account* → *Account API tokens*), not user tokens. The two permissions go on
the token's **zone** policy (scope: the zone), not an *Entire account* policy: Cloudflare refuses the zone calls otherwise. In
the dashboard of 2026-10, *Zone Settings* is under *DNS & Zones* and *Zone SSL & Certificates* under *Cache & Performance*.

1. **Advanced Certificate Manager** on each zone, `lake.live` and `oramadev.net`: the zone → *SSL/TLS* → *Edge
   Certificates* → *Advanced Certificate Manager*. A paid subscription per zone.
2. **The `lake.live` token** (its value is in Secret `lake/cloudflare-api-token-lake-live` on the managing cluster): *My
   Profile* → *API Tokens* → edit the token, zone resource `lake.live` only, and make its permissions:
   - Zone > DNS > Edit (already there)
   - Zone > Zone > Read (already there)
   - **Zone > Zone Settings > Edit** (add)
   - **Zone > SSL and Certificates > Edit** (add)
3. **The `oramadev.net` token** (Secret `lake/cloudflare-api-token`): the same edit, zone resource `oramadev.net` only;
   add **Zone > SSL and Certificates > Edit** (it already has the others).

Editing a token's permissions keeps its value: **no Secret changes and nothing is applied to a cluster.** Keep each token
scoped to its own zone (`oramadev.net` is nonprod, `lake.live` is production).

## After (anyone, read-only)

- Within ten minutes each policy re-reads the zone. Check the conditions above: `Credentials True`, then
  `EdgeCertificateOrders True` with one pack per apps wildcard (the pack can take some minutes to go active).
- `curl -sS -o /dev/null -w '%{http_code}\n' https://<app host>/` answers an HTTP status instead of a handshake error.
- On `lake.live`, `AuthenticatedOriginPulls`, `OriginTLSStrict` and `AlwaysUseHTTPS` move to `True` as their checks pass.
- Record it on [lake#798](https://github.com/oramasearch/lake/issues/798) and in techops-docs
  ([`docs/platform/changes.md`](https://github.com/oramasearch/techops-docs/blob/main/docs/platform/changes.md)), mark
  this runbook's banner done, and close the issue with the result.

## Alternative without ACM (not chosen)

An operator change could put app hosts one level below the zone (for example `<app>-eu.lake.live`), which Universal SSL
covers. It needs code, a change of every app's address, and does not fix the `lake.live` zone-settings permission.
