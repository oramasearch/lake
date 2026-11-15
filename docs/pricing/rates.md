# Lake price list: rates, references and the order sheet fees

Internal: the rates published on Orama's own deployment, their references and margins, worked bills,
the order sheet's fees and the shared tier's promise. Moved here from `docs/api/billing.md` so the
public API reference carries none of it; this folder is not part of the public export.

## Order sheet subscriptions

lake#831 ([orders.md](orders.md)). The apex order sheet sells monthly subscriptions in USD: a
private warehouse ($1,000, including $400 of usage credit), a dedicated cluster ($5,000, including
$2,000), and on-prem from $10,000 (40% of the fee as credit), from the order sheet configuration
(`LAKE_ORDER_SHEET`), not from this price list. Stripe Checkout bills the fee itself (subscription
mode, the offer's Lake [product](#stripe) at the price inline). Each order records `amount_cents` and `included_credit_cents`.

**Not built yet: the credit on the monthly bill.** The intended rule (Steve, 2026-10-18) is that
the month's usage is priced at this price list, the included credit is applied first, and only the
rest is billed, as usage lines after a credit line. Today lake's monthly invoice knows nothing of an
order: an order is anonymous and the warehouse or cluster it provisions is not yet linked to a lake
account (lake#758, lake#832), so the credit cannot be applied. Until then support applies it by
hand. Whether the fees become plans on this list, the credit's percentage, whether unused credit
expires, and what it covers on-prem are decisions still open in lake#831.

## The shared tier's promise

*"Train and serve a model in 5 minutes for under $5"* (Steve, 2026-10-18) leads the shared option
only when `LAKE_ORDER_SHEET`'s `shared.show_promise` is `true`. It is **off**, because the test that
backs it has not been run. The test, on lake.live against the published price list: upload a CSV,
train a model, serve it, call `predict` once from outside, time the whole run, and read
`bill_preview` for the month; it passes when the run takes at most five minutes and the preview
comes to under $5. Record each run here (date, Lake revision, time, cost, the price list id), and
switch the line on only while the latest run passes.

| Date | Revision | Time | Cost (USD) | Price list | Result |
|---|---|---|---|---|---|
| — | — | — | — | — | Not run yet |

## Price list

Orama Cloud's rates ([lake#623](https://github.com/oramasearch/lake/issues/623), Steve,
2026-10-11: *"come up with sensible prices based on market rates for similar services"*; usage only
since [lake#635](https://github.com/oramasearch/lake/issues/635); compute as CPU, RAM, GPU and GPU
memory since [lake#643](https://github.com/oramasearch/lake/issues/643)). Each rate is a public list
price for the same resource, read on 2026-10-11 in US regions, on demand, times a stated margin:
about 1.5–3× for infrastructure, for running it as a managed service, and the model provider's rate
plus 20% for model tokens and speech to text. Prices are in USD. lake#623 and lake#643 carry the full
research tables with each reference's source URL and date.

**Metis is not on this list.** Metis is enterprise software priced by Orama's sales team; a
customer talks to sales. Nothing here prices it.

**Status:** published on lake.oramadev.net on 2026-10-11, effective October 2026. `set_price_list`
needs the operator or the instance administrator; the payload is [below](#publish-the-price-list).
Why the prices are what they are, and how they are governed: [`../billing/pricing-strategy.md`](../billing/pricing-strategy.md).

### Rates

`per` is what the rate counts, in the dimension's unit ([metering.md](metering.md#dimensions)).

| Dimension | Rate | `unit_micros` | `per` | Reference rate (2026-10-11) | Margin |
|---|---|---|---|---|---|
| `cpu` | $0.0015 / vCPU-minute ($0.09 / vCPU-hour) | 1500 | 60000 mCPU-seconds | serverless containers and managed Kubernetes pods, per vCPU-hour: median of eleven public on-demand rates $0.0445 (range $0.024–0.086; hyperscalers $0.040–0.086) | 2.0× |
| `ram` | $0.00017 / GiB-minute ($0.0102 / GiB-hour) | 170 | 61440 MiB-seconds | the same, per GiB-hour of memory: hyperscaler median $0.0050 (range $0.0036–0.0108) | 2.0× ($0.010 / hour is $0.000167 / minute, rounded to $0.00017) |
| `gpu` (`l4`) | $0.0108 / GPU-minute ($0.648 / hour) | 10800 | 60 GPU-seconds | GPU-only on-demand price, CPU and memory billed apart: $0.672 / hour, less its memory share (24 GiB at $0.01) = $0.432 | 1.5× |
| `gpu` (`a10g`) | $0.0215 / GPU-minute ($1.29 / hour) | 21500 | 60 | GPU-only $1.10 / hour, less 24 GiB at $0.01 = $0.86 | 1.5× |
| `gpu` (`a100`) | $0.03875 / GPU-minute ($2.325 / hour) | 38750 | 60 | GPU-only $2.10 / hour (40 GB) and $2.20 (80 GB), less 40 or 80 GiB at $0.01 = $1.70 and $1.40; one class, their mean $1.55 | 1.5× |
| `gpu` (`h100`) | $0.07875 / GPU-minute ($4.725 / hour) | 78750 | 60 | GPU-only $3.95 / hour, less 80 GiB at $0.01 = $3.15 | 1.5× |
| `gpu` (other classes) | $0.03875 / GPU-minute | 38750 | 60 | a GPU of a class without its own rate, or of no known class, is billed as an A100 | — |
| `vram` | $0.00025 / GiB-minute ($0.015 / GiB-hour) | 250 | 61440 MiB-seconds | no provider prices GPU memory apart: it is bundled into each GPU class, at an implied $0.024–0.14 / GiB-hour. Its share here is $0.01 / GiB-hour, below the lowest implied figure, so that each class's compute share above stays positive | 1.5× |
| `storage` | $0.04 / GiB-month | 40000 | 1073741824 bytes | hyperscaler object storage $0.023 / GB-month (the others $0.015–0.020). What Lake holds outside a workspace engine's volume: repositories, container images, transcripts and drive files. What an engine holds on its own volume is `engine_storage`, shown and never billed: the volume is billed, as `volumes` (decision 61, lake#663) | 1.7× |
| `transfer` | $0.05 / GiB | 50000 | 1073741824 bytes | hyperscaler internet egress $0.087–0.12 / GB, ingress free | in and out are one dimension: ~1.1× a half-in, half-out blend, about half of egress alone |
| `speech` | $0.0072 / minute | 7200 | 60 seconds | the model provider's speech-to-text rate, $0.006 / minute | provider + 20%. Speech is billed here only: a transcription model's tokens are never also billed as `model_tokens` (lake#658); the minutes are the model's figure, else measured from the recording ([transcripts.md](transcripts.md#how-long-a-recording-is)) |
| `calls` | $2.00 / million | 2000000 | 1000000 calls | managed HTTP API gateway $1.00 / million (REST APIs $3.50) | 2× |
| `predictions` | $0.01 / 1,000 | 10000 | 1000 calls | serverless CPU inference, about $4.55 / million requests at 2 GB | 2.2× |
| `searches` | $0.01 / 1,000 | 10000 | 1000 calls | the same serverless request reference: a search of a published index is one request served by the workspace engine, whose CPU, RAM and volume are billed apart. Hosted search services charge about $0.50 / 1,000 searches, the index's compute included (lake#658) | 2.2× |
| `model_tokens` (`gpt-6.1-sol/input`, `/output`) | $2.60 / $13.00 a million | 2600000 / 13000000 | 1000000 tokens | OpenAI's published standard price ([pricing](https://developers.openai.com/api/docs/pricing) and the model pages, read 2026-10-26): $2.00 / $10.00. The default model of every workspace's agents (lake#1230) | provider + 30% (Steve, 2026-10-26) |
| `model_tokens` (`gpt-6.1-sol@long/input`, `/output`) | $5.20 / $19.50 a million | 5200000 / 19500000 | 1000000 tokens | a prompt over 272,000 input tokens: 2× input and 1.5× output for the whole request, $4.00 / $15.00 | provider + 30% |
| `model_tokens` (`gpt-6.1-terra/input`, `/output`) | $1.30 / $6.50 a million | 1300000 / 6500000 | 1000000 tokens | $1.00 / $5.00 | provider + 30% |
| `model_tokens` (`gpt-6.1-terra@long/input`, `/output`) | $2.60 / $9.75 a million | 2600000 / 9750000 | 1000000 tokens | $2.00 / $7.50 | provider + 30% |
| `model_tokens` (`gpt-6-astra/input`, `/output`) | $13.00 / $65.00 a million | 13000000 / 65000000 | 1000000 tokens | $10.00 / $50.00 | provider + 30% |
| `model_tokens` (`gpt-6-astra@long/input`, `/output`) | $26.00 / $97.50 a million | 26000000 / 97500000 | 1000000 tokens | $20.00 / $75.00 | provider + 30% |
| `model_tokens` (`gpt-6-luna/input`, `/output`) | $0.13 / $0.65 a million | 130000 / 650000 | 1000000 tokens | $0.10 / $0.50 | provider + 30% |
| `model_tokens` (`gpt-6-luna@long/input`, `/output`) | $0.26 / $0.975 a million | 260000 / 975000 | 1000000 tokens | $0.20 / $0.75 | provider + 30% |
| `model_tokens` (`gpt-5.5/input`, `gpt-5.5/output`) | $6.50 / $39.00 a million | 6500000 / 39000000 | 1000000 tokens | OpenAI's published standard price ([pricing](https://developers.openai.com/api/docs/pricing), read 2026-10-23): $5.00 / $30.00 | provider + 30% (Steve, 2026-10-23) |
| `model_tokens` (`gpt-5.5@long/input`, `/output`) | $13.00 / $58.50 a million | 13000000 / 58500000 | 1000000 tokens | over 272,000 input tokens, read 2026-10-26: $10.00 / $45.00 | provider + 30% (lake#1230) |
| `model_tokens` (`gpt-5/input`, `gpt-5/output`) | $1.625 / $13.00 a million | 1625000 / 13000000 | 1000000 tokens | $1.25 / $10.00 | provider + 30% |
| `model_tokens` (`gpt-4o/input`, `gpt-4o/output`) | $3.25 / $13.00 a million | 3250000 / 13000000 | 1000000 tokens | $2.50 / $10.00 | provider + 30% |
| `model_tokens` (`text-embedding-3-small/input`, `/output`) | $0.026 / $0.026 a million | 26000 / 26000 | 1000000 tokens | $0.02, no output price (an embedding reports no output tokens; the gateway needs both rates) | provider + 30% |
| `model_tokens` (historical unclassed usage only) | $9.00 / million | 9000000 | 1000000 tokens | the model provider's per-token rate for lake's agents' default model: $5.00 / million input, $30.00 output; $7.27 / million at 10 input to 1 output | provider + 20%, rounded up |
| `volumes` | $0.20 / GiB-month | 200000 | 2628000 GiB-seconds (730 hours) | block volume $0.08 / GB-month. Every provisioned volume by its requested size while it runs: a workspace engine's, a managed service's and a store's. An engine's disk is billed this way only, never again for the bytes on it (decision 61, lake#663) | 2.5× |

A GPU-hour, its memory included, is therefore 1.5× its GPU-only reference: an L4 $1.008 ($0.648 +
24 GiB × $0.015), an A10G $1.65, an H100 $5.925; an A100 is $2.925 with 40 GiB (1.39×) and $3.525
with 80 GiB (1.60×). CPU and RAM of the GPU's pod are billed as well, as they are by the GPU-only
references.

**How the references were chosen** (lake#643's research, read 2026-10-11; vendors are named only in
the issue):

- **CPU and RAM.** Separate per-vCPU and per-GiB on-demand rates of serverless container and
  managed Kubernetes services, US regions: CPU $0.024–0.086 per vCPU-hour (median of eleven
  $0.0445), RAM $0.0036–0.0108 per GiB-hour (hyperscaler median $0.0050). Virtual machines split
  lower ($0.022–0.037 per vCPU-hour, $0.0029–0.0045 per GiB-hour); containers carry 1.3–2× over
  them, as lake does.
- **GPU.** The references are GPU-only prices, from providers that bill a GPU by the second with CPU
  and memory billed apart, which is how lake meters it. Whole-instance GPU prices, CPU and memory
  included, run 30–60% above them per GPU (an H100 at $6.15–12.29 per GPU-hour).
- **GPU memory.** Nobody meters GPU memory per GiB; the market prices a GPU class, memory included
  (fractional GPUs are sold by fixed slices). Lake meters it apart (Steve, lake#643), so each class's
  GPU-only reference is split into a memory share at a flat $0.01 per GiB-hour and a compute share,
  the rest.

The compute rate rests on an assumption:

- **Compute is priced on what a workload is scheduled with** (requests, or limits without requests),
  not on what it consumes. A workload that reserves 2 vCPU and idles pays for 2 vCPU, as on
  serverless containers and managed Kubernetes pods; one that asks for nothing is given the
  namespace's default request, and pays for that.

**Long prompts** (lake#1230). OpenAI prices a request whose prompt is longer than 272,000 input
tokens at 2× input and 1.5× output for the whole request (gpt-6.x and gpt-5.5). The gateway records
such a request under `<model>@long/input` and `<model>@long/output`, so it is billed at the `@long`
rates above; 1.3× of the short rate would not cover it. A model with long-context rates is served
only while both its plain and its `@long` rates are published
([model-gateway.md](model-gateway.md#long-prompts)).

**What else the rates cover.** Cached input, which OpenAI bills at 5% of input, is billed at the full
input rate. A cache write, 1.25× input at OpenAI, is billed as input at 1.3×, so it is covered. Lake
never requests regional processing (+10%), which would leave cache writes uncovered (1.25 × 1.1 =
1.375), nor Fast (2×), Ultrafast (6×) or priority processing; the gateway refuses any `service_tier`
but `default` and sends `default` on every request ([model-gateway.md](model-gateway.md#the-processing-tier)).
Batch and Flex (half price) are not used.

New gateway usage prices input and output separately per model. The historical $9 blended
example above and worked bills below describe unclassed usage from before LSR-095; they do not
entitle current gateway requests. Operators must publish both direction rates for approved
models, based on their provider costs; no provider price is inferred or hardcoded.

A workspace's engine runs all month: at the operator's default (64 MiB, and 0.1 vCPU, the
workspace namespace's default request) that is about $6.70 of CPU and $0.47 of RAM a month; at 1
vCPU and 2 GiB, $66.96 and $15.18. There is no free allowance. The monthly `model_tokens` limit is enforced at the model gateway; the
other usage limits warn and do not refuse ([metering.md](metering.md#limits)).

### Worked bills

A 31-day month (744 hours) at the list rates, computed as lake computes a bill (⌈billable ×
`unit_micros` ÷ `per`⌉ per line, each rounded up once to a whole minute of its unit):

| Example | Usage | Bill |
|---|---|---|
| Small | a workspace engine of 1 vCPU and 2 GiB and a Postgres of 0.5 vCPU and 1 GiB all month; 30 build hours at 0.5 vCPU and 1 GiB; 40 GiB stored, 120 GiB transfer, 200 speech minutes, 3 million calls, 200,000 predictions, 3 million tokens, 30 GiB-months of volumes | **$174.90**: CPU $101.79, tokens $27.00, RAM $23.07, transfer $6.00, calls $6.00, volumes $6.00, predictions $2.00, storage $1.60, speech $1.44 |
| Medium | workloads of 6 vCPU and 16 GiB all month; 120 build hours (0.5 vCPU, 1 GiB); an app on one L4 (24 GiB) with 4 vCPU and 16 GiB for 100 hours; 300 GiB, 800 GiB, 1,500 minutes, 25 million calls, 3 million predictions, 15 million tokens, 250 GiB-months | **$1,010.72**: CPU $443.16, RAM $138.96, tokens $135.00, L4 $64.80, calls $50.00, volumes $50.00, transfer $40.00, GPU memory $36.00, predictions $30.00, storage $12.00, speech $10.80 |
| Large | workloads of 24 vCPU and 96 GiB all month; 600 build hours; a training job on one H100 (80 GiB) with 16 vCPU and 128 GiB for 200 hours; 3 TiB, 4 TiB, 10,000 minutes, 200 million calls, 50 million predictions, 80 million tokens, 2,000 GiB-months | **$6,522.48**: CPU $1,922.04, RAM $995.76, H100 $945.00, tokens $720.00, predictions $500.00, calls $400.00, volumes $400.00, GPU memory $240.00, transfer $204.80, storage $122.88, speech $72.00 |

### Revisit

Revisit the price list every quarter, and sooner when:

- a reference rate moves more than 15%, above all the model provider's rate for the default agent
  model, which `model_tokens` passes through, and GPU prices, which fall fastest;
- lake's agents change their default model, or OpenAI changes the long-context threshold (272,000
  input tokens) or its multipliers;
- a GPU class the operator reports has no rate of its own (it is billed at the default rate), or one
  class covers GPUs whose prices differ (`a100` covers 40 and 80 GB);
- `model_tokens` is split into input and output, or into models, so each can carry its own rate.

A change is a new `set_price_list`, normally for the next month ([When prices
change](#when-prices-change)). Issued invoices keep their lines.

### Publish the price list

As the operator, once (amounts in millionths of a dollar, `per` in each dimension's unit; CPU per
vCPU-minute, 60,000 mCPU-seconds; RAM and GPU memory per GiB-minute, 61,440 MiB-seconds; time per
60 seconds, a minute):

```http
POST /v1/set_price_list
{"currency":"USD","prices":[{"dimension":"cpu","unit_micros":1500,"per":60000},{"dimension":"ram","unit_micros":170,"per":61440},{"dimension":"gpu","unit_micros":38750,"per":60},{"dimension":"gpu","class":"l4","unit_micros":10800,"per":60},{"dimension":"gpu","class":"a10g","unit_micros":21500,"per":60},{"dimension":"gpu","class":"a100","unit_micros":38750,"per":60},{"dimension":"gpu","class":"h100","unit_micros":78750,"per":60},{"dimension":"vram","unit_micros":250,"per":61440},{"dimension":"storage","unit_micros":40000,"per":1073741824},{"dimension":"transfer","unit_micros":50000,"per":1073741824},{"dimension":"speech","unit_micros":7200,"per":60},{"dimension":"calls","unit_micros":2000000,"per":1000000},{"dimension":"predictions","unit_micros":10000,"per":1000},{"dimension":"searches","unit_micros":10000,"per":1000},{"dimension":"model_tokens","unit_micros":9000000,"per":1000000},{"dimension":"model_tokens","class":"gpt-5.5/input","unit_micros":6500000,"per":1000000},{"dimension":"model_tokens","class":"gpt-5.5/output","unit_micros":39000000,"per":1000000},{"dimension":"model_tokens","class":"gpt-5/input","unit_micros":1625000,"per":1000000},{"dimension":"model_tokens","class":"gpt-5/output","unit_micros":13000000,"per":1000000},{"dimension":"model_tokens","class":"gpt-4o/input","unit_micros":3250000,"per":1000000},{"dimension":"model_tokens","class":"gpt-4o/output","unit_micros":13000000,"per":1000000},{"dimension":"model_tokens","class":"text-embedding-3-small/input","unit_micros":26000,"per":1000000},{"dimension":"model_tokens","class":"text-embedding-3-small/output","unit_micros":26000,"per":1000000},{"dimension":"model_tokens","class":"gpt-6.1-sol/input","unit_micros":2600000,"per":1000000},{"dimension":"model_tokens","class":"gpt-6.1-sol/output","unit_micros":13000000,"per":1000000},{"dimension":"model_tokens","class":"gpt-6.1-sol@long/input","unit_micros":5200000,"per":1000000},{"dimension":"model_tokens","class":"gpt-6.1-sol@long/output","unit_micros":19500000,"per":1000000},{"dimension":"model_tokens","class":"gpt-6.1-terra/input","unit_micros":1300000,"per":1000000},{"dimension":"model_tokens","class":"gpt-6.1-terra/output","unit_micros":6500000,"per":1000000},{"dimension":"model_tokens","class":"gpt-6.1-terra@long/input","unit_micros":2600000,"per":1000000},{"dimension":"model_tokens","class":"gpt-6.1-terra@long/output","unit_micros":9750000,"per":1000000},{"dimension":"model_tokens","class":"gpt-6-astra/input","unit_micros":13000000,"per":1000000},{"dimension":"model_tokens","class":"gpt-6-astra/output","unit_micros":65000000,"per":1000000},{"dimension":"model_tokens","class":"gpt-6-astra@long/input","unit_micros":26000000,"per":1000000},{"dimension":"model_tokens","class":"gpt-6-astra@long/output","unit_micros":97500000,"per":1000000},{"dimension":"model_tokens","class":"gpt-6-luna/input","unit_micros":130000,"per":1000000},{"dimension":"model_tokens","class":"gpt-6-luna/output","unit_micros":650000,"per":1000000},{"dimension":"model_tokens","class":"gpt-6-luna@long/input","unit_micros":260000,"per":1000000},{"dimension":"model_tokens","class":"gpt-6-luna@long/output","unit_micros":975000,"per":1000000},{"dimension":"model_tokens","class":"gpt-5.5@long/input","unit_micros":13000000,"per":1000000},{"dimension":"model_tokens","class":"gpt-5.5@long/output","unit_micros":58500000,"per":1000000},{"dimension":"volumes","unit_micros":200000,"per":2628000}]}
```

The model rates are also seeded by migrations `0095_model_token_prices` and
`0105_agent_model_prices` (lake#1230) on every instance
([model-gateway.md](model-gateway.md#configuration)); keep them in any list published by hand, or
the gateway refuses those models. It applies from the start of this month; add
`"effective_from": "YYYY-MM"` for another month. Then
`GET /v1/price_list` answers it, and `GET /v1/bill_preview?customer_id=<id>` prices an account's
month at it. Administration → Billing (`/billing/plans`) is the same publication from the web app,
pre-filled with these rates.

