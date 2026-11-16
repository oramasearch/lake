# Client addresses, the proxy listener and trusted proxies

Lake counts several limits per client address: the order sheet's orders
([orders](../api/orders.md#abuse-protection)), Clerk session exchanges
([Clerk](../api/clerk.md)), authentication attempts
([identity](../api/identity.md#authentication-attempt-limits)) and email recovery
([recovery](../api/recovery.md)). This page says which address that is, which peers lake believes,
and how a deployment sets them (lake#1083, lake#1132).

## The rule

For each request lake looks at the listener it arrived on and at the connection's peer, the TCP
address the request arrived from.

- **It arrived on the proxy listener** (`LAKE_PROXY_PORT`): the client is the address the proxy
  reported, whatever the peer, as for a trusted proxy below. Who may reach that port is decided by
  the NetworkPolicy, not by an address range.
- **On the main listener, the peer is a trusted proxy** (inside `LAKE_TRUSTED_PROXY_CIDRS`): the client is the address the
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
orama-operator#720), and lake counts what the ingress decided.

The code is `lake::api::client_address` (`include/lake/api/client_address.hpp`), called from the
HTTP adapter's `order_client_address`; the peer and the listener come from the connection
(`HttpConnections::peer`, `HttpConnections::proxied`).

## `LAKE_PROXY_PORT`: a way in for the proxy alone

| Value | Meaning |
|---|---|
| unset or empty | one listener, as before lake#1132: whether `X-Real-IP` is believed depends on the peer alone (`LAKE_TRUSTED_PROXY_CIDRS`) |
| a port, `1` to `65535` | a second listener on the `--listen` host and this port, serving the same routes with the same workers and limits; `X-Real-IP` / `X-Forwarded-For` are believed on it from any peer |

It may not be the `--listen` port, and a port that is not a number from 1 to 65535 stops lake at
start, naming it. The start log says so
(`lake: per-client limits believe X-Real-IP on the proxy port 4901 from any peer, and on the main
port from none`), and the `startup` event carries `proxy_listen`.

With a proxy listener, an unset `LAKE_TRUSTED_PROXY_CIDRS` means `none`: the main listener believes
no peer, so the only way to name a client address is through the proxy's port.

On a cluster the operator renders both (orama-operator `internal/controller/lake_proxy_listener.go`):
`LAKE_PROXY_PORT` is the container port plus one (4901 by default), `LAKE_TRUSTED_PROXY_CIDRS` is
`none`, the Lake Service exposes the proxy port as `proxy` (8080), the Lake Ingress and LakeApp auth
subrequests use it, and zetta-tunnel's ingress gateway follows the Ingress. The Lake's NetworkSandbox
admits that port from ingress-nginx and the gateway only; every other caller keeps the main port.

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
yet (a setting a new version needs is the operator's, and ships with it).

- **Trusting no one by default would put every visitor in one bucket.** Behind ingress-nginx the
  peer is the ingress pod, which every visitor shares. An image rolled before its operator set the
  variable would rate-limit a whole Lake: after 60 Clerk exchanges in a minute, or 100
  failed sign-ins in fifteen minutes, from anyone, everyone is refused.
- **Inside a cluster every proxy has a private address**, so the default keeps the behaviour lake
  had before lake#1083 for every in-cluster peer.
- **A peer with a public address is never believed by default.** A lake whose port is reached
  directly from the internet (a private install with no proxy in front) counts each client on its
  own address rather than letting it choose its bucket.

### What a CIDR cannot do, and why the proxy listener exists

In a cluster the ingress-nginx pods take their addresses from the same pod range as every other
pod (on the dev AKS cluster, `10.244.0.0/16` for all of them). No CIDR tells the ingress apart from
a workspace pod, so inside a cluster `LAKE_TRUSTED_PROXY_CIDRS` cannot single the ingress out. What
limits the in-cluster peers is the Lake's NetworkPolicy sandbox, rendered by the operator
(`internal/controller/lake_network_sandbox.go`): lake's pods are default-deny for ingress and accept
their HTTP port only from:

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
customer wrote. With one listener such a pod can send `X-Real-IP` from a private address and be
believed. The proxy listener closes that: the proxies have a port of their own that no other
allowance names (`allow-ingress-nginx` alone admits it), and the main port, which every caller
above reaches, believes no peer (`none`). A pod that rotates `X-Real-IP` on the main port is
counted on its own address. Until the operator renders the proxy listener, the per-account and
global budgets still hold such a caller.

### Rollout order

1. **The lake image with the listener rolls first.** Unset `LAKE_PROXY_PORT` keeps today's
   behaviour, so the image changes nothing until the operator sets it.
2. **Then the operator release** that sets `LAKE_PROXY_PORT` and `LAKE_TRUSTED_PROXY_CIDRS=none`
   and points the Ingress, LakeApp auth subrequests and the gateway at the proxy port. Every Lake
   it reconciles must already run an image with the listener: an older image does not open the
   port, and the Ingress would have nothing to send to.

## How to verify

- Unit: `ClientAddress.*` in `tests/test_client_address.cpp` (trusted peer, untrusted peer, unset,
  `none`, parsing, `LAKE_PROXY_PORT` parsing, the default with a proxy listener, the believed form).
- Through the HTTP server:
  `HttpServer.AuthenticationClientBudgetIgnoresForwardedHeadersFromAnUntrustedPeer` sends 100 failed
  recoveries, each with a new `X-Real-IP`. From an untrusted peer the 101st is `rate_limited`; from
  a trusted one a new address still has its budget.
- `OrdersApex.ForwardingHeadersFromAnUntrustedPeerNeverMakeAFreshBucket` (`tests/test_orders.cpp`,
  lake#1006, LSR-200): with `none` on the main listener, ten orders from one peer, each with a new
  `X-Real-IP` and `X-Forwarded-For`, are one client past its limit; the proxy listener still
  counts a new visitor.
- `HttpServer.ForwardedHeadersAreBelievedOnlyOnTheProxyListener`: with `none` on the main listener,
  100 rotated addresses on the proxy port are 100 clients, 100 on the main port are one (the 101st
  is `rate_limited`), and both listeners serve `/healthz`.
  `HttpServer.ProxyListenerIsOptionalAndNeedsItsOwnPort`: unset opens no second listener; a taken
  port fails the start, naming `LAKE_PROXY_PORT`.
- On a pod: the start log line names the proxy port and the ranges.

## How to change it

Set `LAKE_PROXY_PORT` and `LAKE_TRUSTED_PROXY_CIDRS` in the lake container's environment. On a
cluster both are the operator's to render (never by hand on a pod or in a set file; `spec.env`
cannot override them), following the rollout order above.
