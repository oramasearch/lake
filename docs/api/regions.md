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
| `order_sheet_url` | Where the apex order sheet is (lake#831, [orders.md](orders.md#configuration)): `LAKE_ORDER_SHEET`'s `url`, else `https://<LAKE_APEX_HOST>/` on the Lake that serves it; `null` elsewhere |

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

Since lake#831 the apex shows the **order sheet** ([orders.md](orders.md)) around this picker: the
shared option holds exactly the choice below (same preselection, form, `POST`, redirect and cookie),
beside a private warehouse, a dedicated cluster and on-prem with their prices. The sheet adds the
paths `/order`, `/order/<offer>`, `/order/price-list` and `/orders/<id>` on the apex; every other
path behaves as below. `LAKE_ORDER_SHEET` with `{"enabled":false}` gives the plain picker back.

A request whose `Host` (port ignored, any case) equals `LAKE_APEX_HOST` is answered by the picker
before any other route, so no path on the apex reaches the web app or the API. The only exceptions
are the `GET`s of the stylesheet and fonts the page links (`/style-<hash>.css`, `/style.css`,
`/fonts/…`), the probes (`/healthz`, `/readyz`), and the agent documents below.

- **`GET` (any method but `POST`)**: the page (`text/html`, `no-store`), server-rendered by
  `web_ui_order_sheet` (`web_ui_region_picker` when the sheet is off; `src/api/web_ui.cpp`), one
  radio choice per region, working without
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

## The apex's agent documents (lake#1110)

An agent told "use lake.live" reads `https://lake.live/llms.txt` first. The apex is not a Lake, so
it cannot answer a region's text, and the picker's HTML is no use to an agent. A `GET` of one of
the paths a region serves its agent documents at answers, on the apex, the same document written
for every region:

| Path on the apex | Answer | Built by |
|---|---|---|
| `/llms.txt` | `text/plain; charset=utf-8`, `public, max-age=300` | `agent_docs::apex_llms_txt` |
| `/llms-full.txt` | `text/plain; charset=utf-8`, `public, max-age=300` | `agent_docs::apex_llms_full_txt` |
| `/skills/lake/SKILL.md`, `/developer/mcp/skill` | `text/markdown; charset=utf-8`, `public, max-age=300`, `Content-Disposition: attachment; filename=SKILL.md` | `agent_docs::apex_skill_md` |

Each opens with **Choose a region first**: the apex is not a Lake; every region is a separate Lake
with its own accounts, sign-up, API tokens and MCP server; a workspace stays in the region it was
created in and a token from one region does not work in another; use the region where the account
is, and for a new account ask the person which region. Then one section per region of
`LAKE_REGIONS`, in its order, with its MCP server (`https://<host>/mcp`), `llms.txt`,
`llms-full.txt`, skill, sign-in page, token page (`/developer/mcp`), OpenAPI and a `claude mcp add`
command named `lake-<first label of the host>`. After that, the same tool list (llms.txt), tool
reference with input schemas (llms-full.txt) or task map (the skill), worked flows and rules a
region serves, from the same registry. The region list is `LAKE_REGIONS`, the same source the
picker uses: no host is written in the code, and a new region appears with no change to Lake.

Every other path, and a `POST` to these paths (the picker's form), is the picker as above. A
region's own host serves its own documents, which name only that region. With no regions (nonprod)
there is no apex and nothing changes.

The apex serves no `/mcp`, no `/.well-known/` discovery and no `/skills/` index; no region serves
those either today.

```
$ curl -s https://lake.live/llms.txt | head -40
# Lake

> Lake is Orama's platform for designing, training and shipping AI on your own data. It runs as one Lake per region; https://lake.live only lists them. …

## Choose a region first

https://lake.live is not a Lake: it lists Lake's regions, Europe (eu.lake.live) and United States (us.lake.live). …

## Regions

### Europe (`swedencentral`): https://eu.lake.live

- MCP server: `https://eu.lake.live/mcp`
- Agent guide: https://eu.lake.live/llms.txt
…
```

The country centroids are a static table in `src/api/regions.cpp`, taken from Google's Dataset
Publishing Language canonical countries table
(https://developers.google.com/public-data/docs/canonical/countries_csv), kept to current ISO
3166-1 alpha-2 codes (241 entries; its `AN`, `GZ`, `XK` and `UM` rows are left out). No third-party
library is involved.

## How to verify

- `ctest --test-dir build -R Regions` runs `tests/test_regions.cpp`: the variables and every startup
  error, the centroid table, the great-circle distance, the closest region per country, the cookie
  preference, the continue URL keeping the path and query, `server_info` with and without regions,
  the picker over HTTP (every path, the cookie, the `POST`, the refused region, the stylesheet),
  no picker when the variables are unset, and the apex's agent documents
  (`RegionsApexAgentDocuments.*`: every region of the list with every link, the content types,
  the picker still on every other path, a region's own text, nonprod unchanged).
- By hand, with a local Lake:

  ```
  LAKE_REGION=swedencentral LAKE_APEX_HOST=lake.live \
  LAKE_REGIONS='[{"name":"swedencentral","label":"Europe","host":"eu.lake.live","latitude":"60.67488","longitude":"17.14127"},{"name":"eastus","label":"United States","host":"us.lake.live","latitude":"37.3719","longitude":"-79.8164"}]' \
  ./build/lake --memory-store --listen 127.0.0.1:4900
  curl -s -H 'Host: lake.live' -H 'CF-IPCountry: US' 'http://127.0.0.1:4900/w/x?y=1' | grep checked
  curl -si -X POST -H 'Host: lake.live' -d region=eastus 'http://127.0.0.1:4900/w/x?y=1' | grep -i -E 'location|set-cookie'
  curl -s -H 'Host: lake.live' http://127.0.0.1:4900/llms.txt | head -30
  ```

## How to change it

- A new region is a new `LakeSet` member with a region in the operator; Lake reads it from the
  variables and needs no change.
- The order sheet around the picker is [orders.md](orders.md#how-to-change-it).
- The apex's agent documents are `apex_llms_txt`, `apex_llms_full_txt` and `apex_skill_md` in
  `src/api/agent_docs.cpp`; the paths they answer are `apex_agent_document` in
  `src/api/http_server.cpp`.
- The page's words and layout are `web_ui_region_picker`; its styles are `fieldset.region-choices`
  and `label.region-choice` in `src/api/web_ui_styles.inc`.
- The web app's region display is [`../ui/regions.md`](../ui/regions.md).
