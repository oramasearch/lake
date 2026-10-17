# Regions

lake#757, decision 64 ([liquid-bridge `docs/decisions.md`](https://github.com/oramasearch/liquid-bridge/blob/main/docs/decisions.md)).

## What exists

Production Lake runs **one Lake per region**: `eu.lake.live` and `us.lake.live`, managed by one
`LakeSet`. Each region is a separate Lake with its own accounts, workspaces and control plane. The
apex host `lake.live` is a **region picker**: every request to it gets one page that lists the
regions, preselects one, and continues to the chosen region's host with the same path and query.

A picker lists the regions of **one environment only**, never environments. Nonprod has no regions:
nothing below is set there, Lake behaves as it did before, and no picker is shown.

**A workspace belongs to the region of the Lake it was created on.** An account exists in one
region only (there is no shared identity across regions), and moving a workspace between regions is
not supported; both are in the multi-region follow-up epic
(https://github.com/oramasearch/liquid-bridge/issues/40).

## Configuration

Lake has no region setting of its own. The Orama operator sets these environment variables on the
Lake Deployment only when the `LakeSet` member has a region
(https://github.com/oramasearch/orama-operator/issues/512):

| Variable | Example | Meaning |
|---|---|---|
| `LAKE_REGION` | `swedencentral` | This Lake's region: the cloud region name |
| `LAKE_REGION_LABEL` | `Europe` | What a person reads; unset, the label of `LAKE_REGION` in `LAKE_REGIONS`, else the name |
| `LAKE_REGIONS` | `[{"name":"swedencentral","label":"Europe","host":"eu.lake.live","latitude":"60.67488","longitude":"17.14127"}, …]` | Every region of the deployment, this Lake's own included, in order. `latitude` and `longitude` are strings (numbers are accepted too) |
| `LAKE_APEX_HOST` | `lake.live` | Set on exactly one Lake of the deployment, whose ingress also answers this host. A request whose `Host` is this host gets the picker |

`src/main.cpp` reads them through `regions_from_environment` (`include/lake/api/regions.hpp`).
**An invalid value stops startup** with exit code 2 and a message naming the variable:

| Value | Message |
|---|---|
| `LAKE_REGIONS` not JSON | `lake: LAKE_REGIONS is not valid JSON` |
| not an array, an entry not an object | `LAKE_REGIONS must be a JSON array of regions`, `LAKE_REGIONS[0] must be an object` |
| an entry without `name`, `label` or `host` | `LAKE_REGIONS[0].host must be a non-empty string` |
| a `name` other than letters, digits, `-` and `_` | `LAKE_REGIONS[0].name must be letters, digits, hyphens and underscores, …` (the name is the cookie value) |
| a `host` with a scheme, path or anything but letters, digits, `.`, `-` and `:` | `LAKE_REGIONS[0].host must be a host name such as eu.lake.live, …` |
| a coordinate that is not a number, or out of range | `LAKE_REGIONS[0].latitude must be a number, not "north"`, `… must be between -180 and 180` |
| two entries with one name | `LAKE_REGIONS names the region "eu" twice` |
| `LAKE_REGION` not in a non-empty `LAKE_REGIONS` | `LAKE_REGION "westus2" is not one of the regions in LAKE_REGIONS` |
| `LAKE_APEX_HOST` without `LAKE_REGIONS` | `LAKE_APEX_HOST needs LAKE_REGIONS: the picker lists them` |

The startup log line carries `region` and `apex_host` (null when unset).

## `server_info`

`server_info` reports the region through the capability registry, so `GET /v1/server_info`, the
OpenAPI document and the MCP tool all carry the same two fields:

| Field | Value |
|---|---|
| `region` | `{name, label}` of this Lake, or `null` when `LAKE_REGION` is unset |
| `regions` | `[{name, label, host}]`, every region of the deployment in the operator's order, this one included; `[]` when unset |

Coordinates are not reported; they serve only the picker. Both fields are additions: a client that
does not know them reads the rest of `server_info` as before.

```
$ curl -s https://eu.lake.live/v1/server_info | jq '.value | {region, regions}'
{
  "region": {"name": "swedencentral", "label": "Europe"},
  "regions": [
    {"name": "swedencentral", "label": "Europe", "host": "eu.lake.live"},
    {"name": "eastus", "label": "United States", "host": "us.lake.live"}
  ]
}
```

## The apex picker

A request whose `Host` (port ignored, any case) equals `LAKE_APEX_HOST` is answered by the picker
before any other route, so no path on the apex reaches the web app or the API. The only exceptions
are the `GET`s of the stylesheet and fonts the page links (`/style-<hash>.css`, `/style.css`,
`/fonts/…`) and the probes (`/healthz`, `/readyz`).

- **`GET` (any method but `POST`)**: the page (`text/html`, `no-store`), server-rendered by
  `web_ui_region_picker` (`src/api/web_ui.cpp`), one radio choice per region, working without
  JavaScript. Its form posts back to the request's own path and query. The preselected region is:
  1. the region the visitor chose before: the `lake_region` cookie, when it names a region of this
     deployment;
  2. else the region closest to the country Cloudflare sends in `CF-IPCountry`: the smallest
     great-circle distance (haversine, mean Earth radius 6,371.0088 km) from the country's centroid
     to each region's `latitude` and `longitude`;
  3. else (no header, `XX`, `T1`, or a code not in the table) the first region of `LAKE_REGIONS`.
- **`POST` with `region=<name>`** (`application/x-www-form-urlencoded`): `302` to
  `https://<region host><path>?<query>`, the apex request's path and query kept as sent, with
  `Set-Cookie: lake_region=<name>; Path=/; Max-Age=31536000; HttpOnly; SameSite=Lax` (and `Secure`
  behind TLS). The cookie is set on the apex only and holds the region name, never a session.
  A name that is not a region of this deployment answers `400` with the page again and sets nothing.

The country centroids are a static table in `src/api/regions.cpp`, taken from Google's Dataset
Publishing Language canonical countries table
(https://developers.google.com/public-data/docs/canonical/countries_csv), kept to current ISO
3166-1 alpha-2 codes (241 entries; its `AN`, `GZ`, `XK` and `UM` rows are left out). No third-party
library is involved.

## How to verify

- `ctest --test-dir build -R Regions` runs `tests/test_regions.cpp`: the variables and every startup
  error, the centroid table, the great-circle distance, the closest region per country, the cookie
  preference, the continue URL keeping the path and query, `server_info` with and without regions,
  the picker over HTTP (every path, the cookie, the `POST`, the refused region, the stylesheet), and
  no picker when the variables are unset.
- By hand, with a local Lake:

  ```
  LAKE_REGION=swedencentral LAKE_APEX_HOST=lake.live \
  LAKE_REGIONS='[{"name":"swedencentral","label":"Europe","host":"eu.lake.live","latitude":"60.67488","longitude":"17.14127"},{"name":"eastus","label":"United States","host":"us.lake.live","latitude":"37.3719","longitude":"-79.8164"}]' \
  ./build/lake --memory-store --listen 127.0.0.1:4900
  curl -s -H 'Host: lake.live' -H 'CF-IPCountry: US' 'http://127.0.0.1:4900/w/x?y=1' | grep checked
  curl -si -X POST -H 'Host: lake.live' -d region=eastus 'http://127.0.0.1:4900/w/x?y=1' | grep -i -E 'location|set-cookie'
  ```

## How to change it

- A new region is a new `LakeSet` member with a region in the operator; Lake reads it from the
  variables and needs no change.
- The page's words and layout are `web_ui_region_picker`; its styles are `fieldset.region-choices`
  and `label.region-choice` in `src/api/web_ui_styles.inc`.
- The web app's region display is [`../ui/regions.md`](../ui/regions.md).
