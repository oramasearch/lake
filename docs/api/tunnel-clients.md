# Tunnel Client Downloads

Lake issue #745, part 2. A person granted
Developer Access runs the tunnel client, `zetta-tunnel`, on their own machine
([tunnel credentials](tunnel-grants.md)). Lake hands the client out itself, so the Get credentials
drawer can give a working install line instead of assuming the client is installed.

## Where the Clients Come From

The clients are built by `oramasearch/zetta-tunnel`'s `tools/build-clients.sh` and published as the
private scratch image `oramasearch/zetta-tunnel-clients` (pinned: `v0.1.0`) (zetta-tunnel's
`docs/image.md`, *Tunnel clients for developers*). Lake's `Dockerfile` copies its `/clients/` into
the runtime image, pinned by digest, at `/usr/share/lake/tunnel-clients/`:

| File | What it is |
|---|---|
| `zetta-tunnel-darwin-arm64` | macOS on Apple silicon; OpenSSL and nghttp2 linked statically |
| `zetta-tunnel-linux-amd64` | Linux on x86-64; fully static |
| `SHA256SUMS` | `<sha256>  <file>` per client, as `sha256sum` writes it |
| `VERSION` | the clients' version, `0.1.0` in the pinned image (zetta-tunnel `v0.1.0`) |

`LAKE_TUNNEL_CLIENTS_DIR` or `--tunnel-clients-dir` names another folder
([configuration](../serving/configuration.md)). A Lake whose folder is missing (a local build) runs
as before and reports the clients unavailable.

Only the allow-listed platforms are ever offered or served: `darwin-arm64` and `linux-amd64`
(`tunnel_client_platforms()` in `src/api/tunnel_clients.cpp`). A platform's file is offered only
when it is a regular, non-empty file with a line in `SHA256SUMS`. macOS on Intel, Linux on ARM and
Windows are not built yet.

## `tunnel_client_downloads`

| | |
|---|---|
| Kind | read: `GET /v1/tunnel_client_downloads`, the MCP tool of the same name, its OpenAPI operation |
| Input | none (`{}`) |
| Who | any signed-in person: the policy object is the caller's own principal, as `my_permissions` |
| Audit | central log (`audit_routing.cpp`): the links name no workspace's data |

```json
{
  "available": true,
  "version": "0.1.0",
  "expires_at": "2026-10-15T12:15:00Z",
  "clients": [
    {
      "platform": "darwin-arm64",
      "os": "darwin",
      "arch": "arm64",
      "file_name": "zetta-tunnel-darwin-arm64",
      "sha256": "<64 lowercase hex>",
      "size": 9437184,
      "download_url": "https://lake.example/tunnel-clients/darwin-arm64?expires=1792066500&signature=<base64url>"
    }
  ]
}
```

`download_url` is absolute: lake's external origin (`LAKE_EXTERNAL_ORIGIN`), else
`https://<LAKE_PUBLIC_HOST>`, else the origin the request came to, as `list_packages` builds its
commands. Each link works for **15 minutes** (`expires_at`) and needs no session, so `curl` follows
it as is.

`available` is `false`, with `version` and `expires_at` `null` and no clients, when the folder is
missing or holds no usable client, or when the Lake has no `LAKE_INSTANCE_KEK` to sign links with.

## The Signed Link

A link signs `lake-tunnel-client`, the platform, the clients' version and the expiry (Unix seconds),
one per line, with HMAC-SHA-256 under `LAKE_INSTANCE_KEK`, written base64url without padding. That
is the mechanism the Hub's basic-LFS upload links already used (`signed_link_signature` in
`src/api/tunnel_clients.cpp`, now shared by both); the first line keeps a tunnel client signature
from ever standing for another kind of link. Because the version is signed, a link made before the
clients are replaced stops working rather than serving bytes its `sha256` does not describe.

`GET /tunnel-clients/<platform>?expires=<unix seconds>&signature=<signature>` checks, in order:

| Check | Answer |
|---|---|
| the platform is on the allow-list | else `404 not_found`, before anything is read |
| this Lake has clients | else `404 not_found` |
| the signature matches the platform, version and expiry | else `401 unauthenticated`, `reason: "invalid"` |
| the expiry has not passed | else `401 unauthenticated`, `reason: "expired"` |
| the platform's file is present | else `404 not_found` |

A good link is answered `200` with the bytes, `Content-Type: application/octet-stream`,
`Content-Disposition: attachment; filename="zetta-tunnel-<platform>"`, `Content-Length`,
`Cache-Control: no-store`, `X-Content-Type-Options: nosniff` and an `ETag` of the SHA-256. Errors
are the registry's envelope ([errors](errors.md)). The route is in `GET /v1/openapi.json` as
`tunnel_client_download`, with no security requirement: the link is the credential.

## In the Console

Network → Developer Access → Get credentials reads `tunnel_client_downloads` once the certificate
is issued and makes installing the client step 4 ([UI](../ui/network.md#get-credentials),
[guide](../guide/network.md#get-your-tunnel-credentials)).

## Verify

- `tests/test_tunnel_clients.cpp`: the folder (allow-listed files with sums, missing folder,
  version or sum); the allow-list; a link bound to its platform, version, expiry and key;
  `tunnel_client_downloads` answering 15-minute links, unavailable without clients or a key, and
  refused without a credential; the route serving the signed file with its headers, and refusing
  an expired, tampered, unsigned or swapped-platform link (`401`) and an unknown platform or a
  path-like name (`404`), and a Lake without clients.
- `frontend/tests/network.test.ts`: the answer read as clients, the machine detected from the
  browser, and the install paste.
- By hand: `curl -fsSL "<download_url>" -o zetta-tunnel` against a Lake started with
  `--tunnel-clients-dir`, then `shasum -a 256 -c` against the answered `sha256`.

## Change It

- A new platform: the client in the zetta-tunnel-clients image, a row in
  `tunnel_client_platforms()`, the `enum` in the capability's output schema and the OpenAPI route,
  and `CLIENT_PLATFORMS` in `frontend/src/pages/network/network.ts`.
- A new client version: a new zetta-tunnel-clients image and its digest in `Dockerfile`.
