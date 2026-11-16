# What liquid clients check

**What.** One public call says which liquid clients a Lake works with, and a request from a client
outside that range is refused with a clear error instead of failing part-way through a sync
(lake#1324). The clients are the liquid CLI and liquify, the IDE, which links libliquid.

**Why.** Lake is released as open source, so people run their own Lakes at different versions
beside the hosted ones. liquid has to choose a Lake and check it before syncing
([liquid#525](https://github.com/oramasearch/liquid/issues/525)). Before this change `server_info`
said only `api: v1` and a `revision` hash, which a client cannot compare with anything.

## `server_info` → `liquid`

`GET /v1/server_info` needs no credential. Its `liquid` object is:

| Field | Value | Meaning |
|---|---|---|
| `sync_protocol` | `1` | The [sync protocol](sync.md) version Lake serves: `application/vnd.liquid.sync-v1` bodies whose header frame carries `v: 1`. |
| `sync_media_type` | `application/vnd.liquid.sync-v1` | The content type of that protocol's requests and responses. |
| `forge_api` | `1` | The [forge API](forge.md) version: the repository, review, comment, issue and release capabilities under `/v1` that liquid's forge client and liquify call. |
| `oldest_release` | `0.1.0` | The oldest liquid release this Lake syncs with. |
| `newest_release` | the libliquid Lake was built against | The newest liquid release this Lake supports. |
| `version_header` | `Liquid-Version` | The request header a client names its release in. |

```sh
curl -s https://<lake>/v1/server_info | jq .value.liquid
```

The values come from `include/lake/api/liquid_clients.hpp`. `newest_release` is
`liquid::current_version()` of the libliquid Lake links, so it moves when Lake's liquid pin moves to
a new release; nobody edits it by hand. `oldest_release` moves only when Lake stops accepting
something an older liquid sends, and that change says so in its release notes.

## The `Liquid-Version` header

A client sends its own release on every request: `Liquid-Version: 0.2.0`. A leading `v` and a
pre-release or build suffix (`0.2.0-rc.1`, `0.2.0+abc`) are accepted; the suffix is ignored. A
request without the header is served as before, so a liquid that does not send it yet, a script and
a browser are unaffected.

A request whose header names a release outside `[oldest_release, newest_release]`, or a value that
is not `MAJOR.MINOR.PATCH`, is refused before any route runs, with HTTP 400 and the code
`unsupported_client_version` ([errors](errors.md)). The message names the client's release, the
range, and the side to upgrade:

```json
{"ok": false, "error": {"code": "unsupported_client_version", "path": "",
 "message": "liquid 0.3.0 is newer than this Lake supports (liquid 0.1.0 to 0.2.0); use liquid 0.2.0 or older, or ask the Lake's administrator to upgrade it"}}
```

- A sync path (`/v1/repositories/<id>/sync/...`) answers in the sync protocol's own error body
  (`application/vnd.liquid.error-v1`, liquid's `unsupported` code, context
  `unsupported_client_version`) with the same message, so liquid shows it as it shows any sync
  refusal.
- `GET /v1/server_info`, `/healthz` and `/readyz` are never refused, so a client can always read the
  range it is being refused for.

## What waits for liquid

liquid does not send `Liquid-Version` yet, and it does not read `server_info` before syncing. Both
are [liquid#525](https://github.com/oramasearch/liquid/issues/525), which also adds named lakes
(`liquid lake add`). Until it lands, the refusal is reached only by a client that sends the header.

## How to verify

- `Registry.ServerInfoNeedsNoAuth` and `LiquidClients.TheRangeRunsFromTheFirstReleaseToTheLinkedLiquid`
  (`tests/test_api.cpp`) read the versions from the code and check the range and every refusal
  message.
- `HttpServer.ALiquidClientOutsideTheSupportedRangeIsToldSo` (`tests/test_http_server.cpp`) sends
  the header over HTTP: `server_info` answers, `/v1/viewer` is refused with the code and message, an
  in-range release and no header are served, and a sync path answers in the sync error body.
- By hand: `curl -s -H 'Liquid-Version: 99.0.0' https://<lake>/v1/viewer` answers 400
  `unsupported_client_version`.

## How to change it

- A new sync protocol or forge API version: change the constant in `liquid_clients.hpp`, the routes
  that serve it, and this page together. A Lake that serves two versions at once lists both; that
  shape is decided when it happens.
- Dropping support for an old liquid: raise `oldest_supported_liquid` and say so in the release
  notes.
