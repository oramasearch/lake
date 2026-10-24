# Client addresses and trusted proxies

Lake counts several limits per client address: the order sheet's orders
([orders](../api/orders.md#abuse-protection)), Clerk session exchanges
([Clerk](../api/clerk.md)), authentication attempts
([identity](../api/identity.md#authentication-attempt-limits)) and email recovery
([recovery](../api/recovery.md)). This page says which address that is, which peers lake believes,
and how a deployment sets them (lake#1083).

## The rule

For each request lake looks at the connection's peer, the TCP address the request arrived from.

- **The peer is a trusted proxy** (inside `LAKE_TRUSTED_PROXY_CIDRS`): the client is the address the
  proxy reported. That is `X-Real-IP`, which ingress-nginx always writes itself, overwriting any a
  sender wrote. Without it, the last `X-Forwarded-For` hop, the one the nearest proxy appended.
  With neither, the proxy's own address.
- **Any other peer**: the client is the peer's address, whatever `X-Real-IP` or `X-Forwarded-For`
  it sent. A caller that reaches lake's Service without passing the ingress cannot name a new
  address with each request to get a fresh budget.

Lake never reads `CF-Connecting-IP` or the first `X-Forwarded-For` hop: a sender that reaches the
ingress without passing Cloudflare can write either. Which visitor address to believe is the
ingress's decision (ingress-nginx's `forwarded-for-header: CF-Connecting-IP` with
`proxy-real-ip-cidr` limited to Cloudflare's ranges,
https://github.com/oramasearch/orama-operator/issues/720), and lake counts what the ingress decided.

The code is `lake::api::client_address` (`include/lake/api/client_address.hpp`), called from the
HTTP adapter's `order_client_address`; the peer comes from the connection
(`HttpConnections::peer`).

## `LAKE_TRUSTED_PROXY_CIDRS`

| Value | Meaning |
|---|---|
| unset or empty | loopback and the private ranges: `127.0.0.0/8`, `::1/128`, `10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`, `100.64.0.0/10`, `fc00::/7`, `fe80::/10` |
| comma-separated CIDRs | only peers inside one of them; a bare address is a `/32` or `/128`; IPv4-mapped IPv6 peers (`::ffff:a.b.c.d`) match IPv4 ranges |
| `none` | no peer is a proxy: every request is counted on its peer address |

A malformed entry (not an address, a bad prefix length, host bits set past the prefix) stops lake
at start with an error naming it. Lake logs the ranges it believes once at start:
`lake: per-client limits believe X-Real-IP from ...`.

### Why the default is the private ranges

The default must not change anything for a deployment whose operator does not set the variable
yet (the umbrella rule: a setting a new version needs is the operator's, and ships with it).

- **Trusting no one by default would put every visitor in one bucket.** Behind ingress-nginx the
  peer is the ingress pod, which every visitor shares. An image rolled before its operator set the
  variable would rate-limit lake.live as a whole: after 60 Clerk exchanges in a minute, or 100
  failed sign-ins in fifteen minutes, from anyone, everyone is refused.
- **Inside a cluster every proxy has a private address**, so the default keeps the behaviour lake
  had before lake#1083 for every in-cluster peer.
- **A peer with a public address is never believed by default.** A lake whose port is reached
  directly from the internet (a private install with no proxy in front) counts each client on its
  own address rather than letting it choose its bucket.

### What the default does not do

In a cluster the ingress-nginx pods take their addresses from the same pod range as every other
pod (on the dev AKS cluster, `10.244.0.0/16` for all of them). No CIDR tells the ingress apart from
a workspace pod, so inside a cluster `LAKE_TRUSTED_PROXY_CIDRS` cannot single the ingress out, and
the operator does not set it today. What limits the in-cluster peers is the Lake's NetworkPolicy
sandbox, rendered by the operator (`internal/controller/lake_network_sandbox.go`): lake's pods are
default-deny for ingress and accept their HTTP port only from:

| Caller | Policy |
|---|---|
| ingress-nginx (namespace `ingress-nginx`, `app.kubernetes.io/name: ingress-nginx`) and zetta-tunnel's ingress gateway | `allow-ingress-nginx` |
| the Orama operator | `allow-operator-to-lake` |
| the Lake's library agents | `allow-agent-ingress-to-lake` |
| every pod in the Lake's workspace namespaces (workspace Zettas, apps, services) | `allow-workspace-ingress` |
| BuildKit build Jobs | `allow-build-ingress-to-lake` |
| methodology stage, collect and serve pods | `allow-methodology-ingress-to-lake` |
| the LakeE2ETest runner | `allow-e2e-ingress-to-lake` |

Each of these calls lake legitimately, so the policy cannot drop them, and several run code a
customer wrote. Such a pod can still send `X-Real-IP` from a private address and be believed. That
gap is closed by giving proxied traffic a way in of its own (a listener only the ingress may reach),
not by a CIDR; until then, the per-account and global budgets still hold such a caller.

## How to verify

- Unit: `ClientAddress.*` in `tests/test_client_address.cpp` (trusted peer, untrusted peer, unset,
  `none`, parsing).
- Through the HTTP server:
  `HttpServer.AuthenticationClientBudgetIgnoresForwardedHeadersFromAnUntrustedPeer` sends 100 failed
  recoveries, each with a new `X-Real-IP`. From an untrusted peer the 101st is `rate_limited`; from
  a trusted one a new address still has its budget.
- On a pod: the start log line names the ranges.

## How to change it

Set `LAKE_TRUSTED_PROXY_CIDRS` in the lake container's environment. On a cluster that is the
operator's to render (never by hand on a pod or in a set file), in an operator release that rolls
before the lake image that needs a non-default value.
