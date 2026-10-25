# Pricing: the lake.live price list

The complete usage price list for **lake.live**, approved by Steve on 2026-10-24.
lake.live publishes only the 8 model-token rates today: migration `0095_model_token_prices` seeded
them, and `https://lake.live/order/price-list` shows them alone, read 2026-10-24. Every other
dimension (CPU, RAM, GPU, storage, transfer and the rest) has no rate there, so it is not charged.
This is a launch-checklist item of [lake#755](https://github.com/oramasearch/lake/issues/755), carried
by [lake#1158](https://github.com/oramasearch/lake/issues/1158).

**Status: approved by Steve on 2026-10-24, not yet published.** Publishing is an admin-token write
on each production Lake, done separately on Steve's go-ahead (below).

| File | What it holds |
|---|---|
| [`lake-live-price-list.json`](lake-live-price-list.json) | `set_price_list`: the exact request body. `lines`: each rate in dollars with its source and what the website says. `not_priced`: the dimensions left off, and why. The flags for Steve |

How the rates were set and why: [`../billing/pricing-strategy.md`](../billing/pricing-strategy.md).
The price list API, bills and the balance: [`../api/billing.md`](../api/billing.md). Units:
[`../api/metering.md`](../api/metering.md#dimensions).

## The lines

Every rate is `unit_micros` millionths of a dollar for every `per` units of the dimension's unit.
Each one is the rate already in [`billing.md` § Rates](../api/billing.md#rates), which was set in
[lake#623](https://github.com/oramasearch/lake/issues/623) and
[lake#643](https://github.com/oramasearch/lake/issues/643) and published on the development Lake
since 2026-10-11. No rate in this list is new.

| Line | Rate (USD) | `unit_micros` / `per` | What counts | Website (`/pricing`) |
|---|---|---|---|---|
| `cpu` | 0.0015 per vCPU-minute | 1500 / 60000 mCPU-s | the vCPUs each workload is scheduled with (engines, services, apps, builds, runs, agents) | same |
| `ram` | 0.00017 per GiB-minute | 170 / 61440 MiB-s | the memory each workload is scheduled with | same |
| `gpu` (`l4`) | 0.0108 per GPU-minute | 10800 / 60 | GPU time on an L4 | same |
| `gpu` (`a10g`) | 0.0215 per GPU-minute | 21500 / 60 | on an A10G | same |
| `gpu` (`a100`) | 0.03875 per GPU-minute | 38750 / 60 | on an A100 | same |
| `gpu` (`h100`) | 0.07875 per GPU-minute | 78750 / 60 | on an H100 | same |
| `gpu` (no class) | 0.03875 per GPU-minute | 38750 / 60 | any other or unknown GPU class, billed as an A100 | same ("other") |
| `vram` | 0.00025 per GiB-minute | 250 / 61440 MiB-s | GPU memory | same |
| `storage` | 0.04 per GiB-month | 40000 / 1 GiB | what Lake holds outside an engine's volume: repositories, images, transcripts, drive files, registry packages (decisions 61, 62); the average of the month's daily readings | same |
| `transfer` | 0.05 per GiB | 50000 / 1 GiB | bytes in and out | same |
| `speech` | 0.0072 per minute | 7200 / 60 s | speech to text, by audio length | same |
| `calls` | 2.00 per million | 2000000 / 1000000 | API and MCP calls routed to a workspace | same |
| `predictions` | 0.01 per thousand | 10000 / 1000 | answered `predict` calls | same |
| `searches` | 0.01 per thousand | 10000 / 1000 | answered searches of a published index ([lake#658](https://github.com/oramasearch/lake/issues/658)) | **not listed** |
| `model_tokens` (`gpt-5.5/input`, `/output`) | 6.50 / 39.00 per million | 6500000 / 39000000 per 1000000 | model gateway tokens, by model and direction | same; already live |
| `model_tokens` (`gpt-5/input`, `/output`) | 1.625 / 13.00 per million | 1625000 / 13000000 | | same; already live |
| `model_tokens` (`gpt-4o/input`, `/output`) | 3.25 / 13.00 per million | 3250000 / 13000000 | | same; already live |
| `model_tokens` (`text-embedding-3-small/input`, `/output`) | 0.026 / 0.026 per million | 26000 / 26000 | | same; already live |
| `volumes` | 0.20 per GiB-month | 200000 / 2628000 GiB-s (730 h) | every provisioned volume by its requested size while it runs; an engine's disk is billed only this way (decision 61) | same |

Time is metered by the second and billed on the month's total, rounded up once to a whole minute
([billing.md](../api/billing.md#prices-and-bills)).

**Left off the list** (`not_priced` in the JSON):

- **`model_tokens` with no model class**: `billing.md`'s payload adds a blended $9.00 per million for
  usage recorded before model classes existed (LSR-095). lake.live started after LSR-095, so it has
  no such usage, and the website shows no blended rate. Without a rate, unclassed tokens are not
  charged.
- **`engine_storage`, `instances`, `build_compute`, `run_compute`**: detail, shown on the Usage page
  and never billed. `set_price_list` refuses a rate for them.

**Not on this list at all:** the order sheet's monthly offers (Private Warehouse $1,000 with $400 of
credit, Dedicated Cluster $5,000 with $2,000, On-Prem from $10,000 with 40%). They come from
`LAKE_ORDER_SHEET` ([orders.md](../api/orders.md#what-exists)) and match the website's
`lakeOffers`. Usage beyond their credit is billed at this list. Metis is priced by sales.

## Website and lake: the differences

Checked on 2026-10-24 against orama-website-v2 branch `live`: `src/content/pages/rates.ts`
(`LAKE_RATES`) and `content/registry/product-truth.json` (`lakePriceList`, `lakeOffers`).

1. **Every rate the website states matches this list exactly**: 22 rates, compared number by number.
2. **`searches`** is billed by lake (0.01 per thousand) but is missing from the website. The
   website's `LakeDimensionId` type has no `searches` row. Either the website adds it, or the rate
   is removed here. Steve decides.
3. **The published list on lake.live is not what the website says.** The website shows the full
   list and links to `https://lake.live/order/price-list` as its source. That page shows only the 8
   model rates. Publishing this list closes the gap.
4. **Unclassed model tokens**: `billing.md` and the console editor's `LIST_RATES` carry the blended
   $9.00, but this list and the website do not (above).
5. **The offers match**: website `lakeOffers` against `docs/api/orders.md`, fees, credit and "from"
   on On-Prem.

## What Steve decided (2026-10-24)

- **The rates: approved**, as listed.
- **`effective_from`: `2026-11`.** October stays billed at today's list (model tokens only).
- **`searches`: kept** at 0.01 per thousand; the website adds it to its pricing page.
- **Unclassed model tokens: not charged.**

## The choices as they were put to him

- **Approve the rates.** They are the development Lake's rates since 2026-10-11, plus the model
  rates of 2026-10-23. None is new.
- **`effective_from`: `2026-11` (in the file) or `2026-10`.** A month is billed at the list in force
  at its end ([When prices change](../api/billing.md#when-prices-change)).
  - **`2026-11`**: October stays billed at today's list (model tokens only), so compute, storage and
    volumes used since launch are free. November onward is priced. `price_list` shows it as `next`,
    and the Billing page announces it.
  - **`2026-10`**: all of October is priced at once, the days since launch included. Every balance
    is refreshed every 5 minutes, so each account that has topped up is charged its compute,
    storage and volumes since 2026-10-24 within minutes, without notice. A workspace engine at the
    default size costs about $7 a month in CPU and RAM.
- **`searches`**: keep it and add it to the website, or drop it.
- **Unclassed model tokens**: leave them uncharged, or add billing.md's $9.00 per million.

## How to publish it, once approved

Lake's prices live in each Lake's own control Postgres, so **each region is published separately**.
Both use the same file. Only the operator credential or an instance administrator may call
`set_price_list` ([billing.md](../api/billing.md#capabilities)).

| Region | Host | Leaf cluster | Lake (namespace) | Admin token |
|---|---|---|---|---|
| Europe (also serves the apex `lake.live` and its `/order/price-list`) | `eu.lake.live` | `orama-production-cluster-02` | `shared` (`lake-shared`) | Secret `lake-shared-admin`, key `token` |
| United States | `us.lake.live` | `orama-production-cluster-03` | `shared-us` (`lake-shared-us`) | Secret `lake-shared-us-admin`, key `token` |

The leaf's kubeconfig is in the managing cluster's Secret `orama-system/<cluster>-kubeconfig`, key
`kubeconfig`. Write it to a private file with mode 600, never into a repository (the umbrella
`lake-devops` skill). The token goes from the Secret to the request without being printed:

```sh
# Run once for each row above: CLUSTER, NS, LAKE and HOST from the table.
CLUSTER=orama-production-cluster-02 NS=lake-shared LAKE=shared HOST=eu.lake.live
KC=$(mktemp) && chmod 600 "$KC"
kubectl --context orama-production-cluster-01 -n orama-system get secret "$CLUSTER-kubeconfig" \
  -o jsonpath='{.data.kubeconfig}' | base64 -d > "$KC"
jq '.set_price_list' docs/pricing/lake-live-price-list.json |
  curl -sS "https://$HOST/v1/set_price_list" \
    -H "Authorization: Bearer $(kubectl --kubeconfig "$KC" -n "$NS" get secret "lake-$LAKE-admin" \
         -o jsonpath='{.data.token}' | base64 -d)" \
    -H 'Content-Type: application/json' --data-binary @-
rm -f "$KC"
```

To publish for October instead, use `jq '.set_price_list | .effective_from = "2026-10"'`.

**Check after each region:**

- `set_price_list` answers `ok: true` with the publication's id (`usage-<YYYYMM>-…`).
- `GET https://<host>/v1/price_list` (signed in) shows 23 prices under `list` (October) or `next`
  (November).
- On the EU region, `https://lake.live/order/price-list` shows every line, once the list is in force
  for the month it shows.
- `bill_preview` for a test account prices its month at the new list.

**Record it** (umbrella `CLAUDE.md`, *Every change to the stack is recorded*): the publication ids
and the time in techops-docs `docs/lake/changes.md`, the change record in compliance, and a
comment on lake#755 and lake#1158. Then tick the item on both.

## How to change

New rates: change the JSON (`set_price_list` and its `lines`), the table above and
[`billing.md` § Rates](../api/billing.md#rates) together, then publish a new list, normally for the next month. Issued invoices keep their lines.
When a rate here changes, change `rates.ts` and `product-truth.json` in orama-website-v2 in the same
round, so `/pricing` and lake.live keep saying the same thing.
