# Billing: plans, bills and invoices

Billing is Orama Cloud's (lake.live): a price list of plans, each account's plan, the bill for a
month worked out from the account's metered usage, and the invoices lake issues with their payment
state ([lake#564](https://github.com/oramasearch/lake/issues/564)). It is **off on enterprise
installs**, where usage tracking and its charts stay on ([metering.md](metering.md#usage-over-time)).

## Where it lives (decision 56)

[Decision 56](https://github.com/oramasearch/liquid-bridge/blob/main/docs/decisions.md)
([lake#617](https://github.com/oramasearch/lake/issues/617)), Steve, 2026-10-10: *"billing records
go in postgres... usage records go in zetta workspace."*

- **Billing records** are multitenancy, in lake's control PostgreSQL (migration `0073_billing`,
  [schema](../control-plane/schema.md#billing-migration)): `billing_plans` and
  `billing_plan_prices` (the price list), `billing_subscriptions` (each account's plan from the
  first day of a month), `billing_invoices` (one account, one month, one plan, a total and a payment
  state) and `billing_invoice_lines`. They hold account ids, dimensions, quantities and amounts,
  never workspace content.
- **Usage stays in each workspace Zetta** (`lake_usage`, `lake_storage`). A bill reads it through
  `usage_over_time`, called as the same caller, by day over the month. An invoice's lines freeze the
  quantity billed for each dimension; that number per dimension per month is the only usage that
  reaches PostgreSQL. Decision 55 dropped the central usage history; nothing here restores it.
- Every billing capability is policy-checked and audited in the **central** log (decision 52,
  [audit.md](audit.md#which-log)): billing is account configuration, not a workspace's data.

## The enterprise switch

`LAKE_BILLING` turns billing on: `on`, `true` or `1`. Anything else, unset included, is off. Orama's
own lake sets it in the Lake CR's `spec.env`; an enterprise install leaves it unset.

Off, lake does not register the billing capabilities, so they are absent from REST `/v1`, OpenAPI,
MCP `tools/list` and Developer → MCP; `viewer` answers `billing: false`, the web app has no Billing
item or page, and `list_docs`/`get_doc` leave out the Billing guide. A call by name answers
`unknown_capability`, as for any capability lake does not have. Usage, limits and the usage dashboard are
unaffected, and the billing tables (created by the schema everywhere) stay empty.

`LAKE_BILLING_INSTRUCTIONS` is what each invoice of the manual provider tells the account about
paying (bank details, say); unset, it says Orama will send payment details.

## Payment providers

[Decision 57](https://github.com/oramasearch/liquid-bridge/blob/main/docs/decisions.md)
([lake#620](https://github.com/oramasearch/lake/issues/620)), Steve, 2026-10-11: *"let's use stripe
for now"*. Lake collects payment through `PaymentProvider` (`include/lake/api/billing.hpp`), an
interface with:

- `name()` and `title()`: stored on each invoice (`provider`) and shown by `billing_account`;
- `provider_customer(account)`: the provider's id for the account, created the first time; lake
  keeps it (`billing_accounts`) before it asks for payment, so a failed request never loses it;
- `request_payment(account, invoice)`: called for an invoice about to be issued (its account, month,
  plan, lines and total, and which issue of the month it is). It answers the provider's `reference`,
  the `instructions` the invoice shows, the `payment_url` where the account pays and the provider's
  `provider_status`, or an error, which leaves the invoice unissued (`store_unavailable`);
- `cancel_payment(invoice)`: called by `void_invoice` before lake voids a due invoice, so it can no
  longer be paid; an error leaves it due;
- `read_notice(body, signature, now)`: checks and reads a payment notice the provider sent;
- `records_payments_by_hand()`: whether the operator records each payment with
  `record_invoice_payment` (true for the manual provider; false for Stripe, which reports payments
  itself).

Lake ships two implementations. **Stripe** ([below](#stripe)) where lake was started with its keys,
and **manual** ("Invoice only") everywhere else: lake issues the invoice, Orama collects
payment outside lake, and the operator records it. A provider is lake's own code against the
provider's HTTP API through lake's outbound client, never a third-party SDK (liquid-bridge
`CLAUDE.md`, "We write our own"). Tests inject a fake through `register_billing_capabilities` or
`ServerOptions::payment_providers`.

Which provider is in use is decided when lake starts, from its environment (`PaymentProviders`,
lake#628): the billing capabilities and the notice route ask for the current one, and a key
changed, rotated or removed takes effect at the next restart. `record_invoice_payment` goes by the invoice's own
provider: an invoice issued by the manual provider stays recordable after Stripe is set up, and a
Stripe invoice never is (`conflict`).

`billing_account` answers `provider: {name, title, records_payments_by_hand, online_payment,
reason}`; `reason` says why card payment is not available while the manual provider is in use.
`viewer.billing_payment` (`{provider, title, online_payment, reason}`, null where billing is off)
and `server_info` (`billing`, `payment_provider`, `online_payment`) report the same. None of them
ever carries a key.

## Stripe

**Customers.** A lake account becomes one Stripe Customer (`POST /v1/customers`, name and
`metadata[lake_customer_id]`) the first time one of its invoices is issued, not when it chooses a
plan, so accounts that never pay leave nothing in Stripe. Its id (`cus_...`) is kept in
`billing_accounts` (customer, provider, provider customer id), the account's billing row in
PostgreSQL (decision 56), and reused for every later invoice.

**Invoices.** `issue_invoice` freezes lake's invoice as before, then:

1. `POST /v1/invoices`: a draft for the customer, `currency=usd`, `collection_method=send_invoice`,
   `days_until_due=30`, `auto_advance=false`, `pending_invoice_items_behavior=exclude`, with the
   month and plan in its description and metadata;
2. `POST /v1/invoiceitems`: one item per lake line with an amount, in cents (millionths rounded half
   up), described as the line is; when the rounded lines do not add up to lake's total rounded to the
   cent, one more item, **Rounding to the cent**, carries the difference;
3. `POST /v1/invoices/<id>/finalize`, whose answer gives `hosted_invoice_url` and `status`.

Lake keeps the Stripe invoice id as `provider_reference`, the hosted page as `payment_url` and
Stripe's status as `provider_status` (`open`). **Lake's invoice stays the source of truth for
amounts**: Stripe's total is lake's to the cent, and the lines are lake's. Stripe bills USD only
here; another currency is refused before anything is sent.

Every request is form-encoded with `Authorization: Bearer <secret key>`, `Stripe-Version:
2024-06-20` (`stripe_api_version`) and an `Idempotency-Key`: `lake-customer-<customer>` for the
customer, `lake-invoice-<customer>-<month>-<n>` for the invoice (n is the issue of the month: 0,
then 1 after a void), with `-line-<i>`, `-rounding` and `-finalize` for the steps after it, and
`lake-void-<invoice>` for a void. A retry of a failed issue within Stripe's 24 hours therefore gets
Stripe's first answers back instead of a second invoice; an issue after a void is a new invoice.
Stripe's error message is kept, except on 401 and 403, where it can echo part of the key.

**Payment.** The account pays on Stripe's hosted invoice page, by card or bank, from **Pay with
Stripe** on lake's invoice page. Stripe tells lake through its webhook ([below](#webhooks)).

**Voids.** `void_invoice` on a due Stripe invoice voids it in Stripe first (`POST
/v1/invoices/<id>/void`); if Stripe refuses (it was paid meanwhile), lake's invoice stays due. A
Stripe invoice cannot be voided while lake has no Stripe keys (`store_unavailable`): void it in
Stripe, and Stripe's notice voids it in lake.

The client is `src/api/stripe.cpp` (`include/lake/api/stripe.hpp`); its HTTP exchange is the
server's outbound client, injected as a `StripeHttp` the tests replace with a fake Stripe.

## Webhooks

`POST /billing/stripe/webhook` receives Stripe's events. It has no lake credential: the
`Stripe-Signature` header is the credential.

1. **Signature.** `Stripe-Signature: t=<unix seconds>,v1=<hex>[,v1=...]`; lake computes
   HMAC-SHA256 of `<t>.<raw body>` with the webhook secret (OpenSSL) and compares it with every
   `v1` in constant time. No match is `400 invalid_signature`. When the check fails, lake reads the
   keys again once (a rotated webhook secret) before refusing.
2. **Replays.** A `t` more than 300 seconds from lake's clock, either way, is `400
   stale_timestamp`: a captured request cannot be replayed later. Within the window, the event id
   decides.
3. **Once per event.** Each applied event is recorded in `billing_payment_events` (provider, event
   id, type, Stripe invoice, lake invoice), in the same transaction that moves the invoice. The same
   event again (Stripe's own retry, or a replay inside the window) answers `200 {"duplicate":
   true}` and changes nothing.
4. **What moves.** Only a due invoice moves:

   | Event | Lake invoice | `provider_status` |
   |---|---|---|
   | `invoice.paid` | `due` → `paid` (`paid_at`) | `paid` |
   | `invoice.payment_failed` | stays `due` | `payment_failed` |
   | `invoice.voided` | `due` → `void` (`voided_at`, note "Voided in Stripe") | `void` |

   A paid invoice is never voided or failed by a later event; a void lake asked for is confirmed
   (`provider_status` becomes `void`). Other event types answer `200 {"ignored": true}`; an invoice
   lake did not issue answers `200 {"matched": false}`.
5. **Audit.** Every event that moves an invoice is a central audit event (decision 52) on
   `customer/<id>/billing`, action `payment_notice`, kind `billing.invoice.paid`,
   `billing.invoice.void` or `billing.invoice.payment_failed`, principal `payment-provider`, with
   the event id and both invoice ids. Refused notices are recorded `denied` on `billing/stripe`,
   unmatched ones `ignored`.

The route answers `404` where billing is off or the manual provider is in use, `413` for a body
over 1 MiB, and `503` (Stripe retries) when the keys or the store cannot be read.

**Endpoint to register in the Stripe dashboard:** `https://<lake host>/billing/stripe/webhook`
(on dev, `https://lake.oramadev.net/billing/stripe/webhook`), with the events `invoice.paid`,
`invoice.payment_failed` and `invoice.voided`. Its signing secret (`whsec_...`) is the
`webhook_secret` below.

## Where the keys live

**The Stripe keys are lake's own platform credentials, environment variables of the hosting
platform** (lake#628, decision 57). Lake has two levels of secrets: Orama's own (Stripe, lake's OpenAI and Clerk
keys, anything lake uses as a service), which are environment variables of the platform lake runs
on, and users' workspace secrets, which live in lake's secret store ([secrets.md](secrets.md)). The
Stripe keys are the first kind and never go in a user's secret store or a secret mount.

| Variable | Holds |
|---|---|
| `LAKE_STRIPE_SECRET_KEY` | The secret key, `sk_test_...` or `sk_live_...`, or a restricted `rk_...` key that may write customers, invoices and invoice items |
| `LAKE_STRIPE_WEBHOOK_SECRET` | The webhook endpoint's signing secret, `whsec_...` |

- **Where they are set.** On Orama's own deployments, as Railway variables of the lake service; on
  the dev cluster, in the Lake CR's `spec.env` with a `valueFrom.secretKeyRef` to a Kubernetes
  Secret in the `lake` namespace, never a literal value ([aks.md](../deployment/aks.md#stripe)).
- **Read once, at start**, and never logged: not at start, not in a request log, not in an error.
  Stripe's refusal of a key says which variable to check, never its value.
- **Rotation is a restart.** Set the new value and roll lake; the new process uses it. Roll the
  webhook secret in Stripe ("Roll secret" keeps the old one valid for a while), set the new
  `whsec_`, and roll lake within that window.
- **Fallback.** Billing on with either variable unset or of the wrong shape (`sk_`/`rk_`,
  `whsec_`): the manual provider, with the reason, which names the variable and never a value, on
  the Billing page (the operator sees the reason; an account sees "Card payment is not set up"), in
  `billing_account`, `viewer.billing_payment` and `server_info`.
- **Billing off** (an enterprise install): no provider, no route, nothing read.

## Prices and bills

A plan has a monthly fee (`base_micros`) and at most one price per usage dimension, keyed on the
names `usage_over_time` answers ([metering.md](metering.md#dimensions)): `unit_micros` for every
`per` units of the dimension's unit, beyond `included` units a month. Amounts are integer millionths
of the plan's three-letter `currency`. A dimension without a price is not charged.

The bill for a month (`YYYY-MM`, a UTC calendar month) is the plan in effect on its first day:

1. a `plan` line: the fee;
2. one `usage` line per priced dimension, by dimension name, with `quantity` (the month's use),
   `included`, `billable` (`quantity` beyond `included`) and `amount_micros` =
   ⌈`billable` × `unit_micros` ÷ `per`⌉, capped at the largest bigint.

`quantity` for a counted dimension is the month's total across the account's live workspaces; for
storage (a reading) it is the average of the month's daily readings, each the sum of every
workspace's latest reading that day. Workspaces deleted before the bill is read are not included
(decision 56's cost).

A plan chosen by an account with no plan applies from the first day of the current month; a change
applies from the next month; choosing the current plan again removes a pending change. There is no
proration.

## Price list

Orama Cloud's prices ([lake#623](https://github.com/oramasearch/lake/issues/623), Steve,
2026-10-11: *"come up with sensible prices based on market rates for similar services"*). Each
unit price is a public list price for the same resource, read on 2026-10-11 in US regions, times a
stated margin: about 1.5–3× for infrastructure, for running it as a managed service, and the
model provider's rate plus 20% for model tokens and speech to text. Prices are in USD. The issue
carries the full research table with each reference's source URL.

**Status:** not yet published on lake.oramadev.net. `set_billing_plan` needs the operator or the
instance administrator; the three payloads are in lake#623 and in [Publish](#publish-the-price-list)
below.

### Unit prices

The list price applies beyond the included amounts on Free and Team; Business pays the lower
overage. `per` is what the price counts, in the dimension's unit
([metering.md](metering.md#dimensions)).

| Dimension | Price (list) | Business overage | `per` | Reference rate (2026-10-11) | Margin |
|---|---|---|---|---|---|
| `storage` | $0.04 / GiB-month | $0.03 | 1073741824 bytes | hyperscaler object storage $0.023 / GB-month (the others $0.015–0.020) | 1.7× |
| `transfer` | $0.05 / GiB | $0.04 | 1073741824 bytes | hyperscaler internet egress $0.087–0.12 / GB, ingress free | in and out are one dimension: ~1.1× a half-in, half-out blend, about half of egress alone |
| `build_compute` | $0.30 / hour | $0.24 | 3600 seconds | serverless containers $0.0405 / vCPU-hour + $0.00445 / GB-hour ($0.099 / hour for 2 vCPU, 4 GiB); hosted CI runner, 2 cores, $0.36 / hour | 3× containers, below the CI runner |
| `run_compute` | $0.24 / hour | $0.20 | 3600 seconds | the same 2 vCPU, 4 GiB container, $0.099 / hour | 2.4× |
| `speech` | $0.0072 / minute | $0.0072 | 60 seconds | the model provider's speech-to-text rate, $0.006 / minute | provider + 20% |
| `calls` | $2.00 / million | $1.50 | 1000000 calls | managed HTTP API gateway $1.00 / million (REST APIs $3.50) | 2× |
| `predictions` | $0.01 / 1,000 | $0.008 | 1000 calls | serverless CPU inference, about $4.55 / million requests at 2 GB | 2.2× |
| `model_tokens` | $9.00 / million | $9.00 | 1000000 tokens | the model provider's per-token rate for lake's agents' default model: $5.00 / million input, $30.00 output; $7.27 / million at 10 input to 1 output | provider + 20%, rounded up |
| `instances` | $0.08 / instance-hour | $0.065 | 3600 instance-seconds | a small managed database or broker instance (2 vCPU, 2 GiB), $0.032–0.046 / hour | ~2× |
| `volumes` | $0.20 / GiB-month | $0.16 | 2628000 GiB-seconds (730 hours) | block volume $0.08 / GB-month | 2.5× |

Two prices rest on an assumption, because a dimension counts less than its reference does:

- **Compute is counted in seconds of a job, not vCPU-seconds** (the operator does not report CPU and
  memory, [lake#612](https://github.com/oramasearch/lake/issues/612)), so a build or run is priced
  as a 2 vCPU, 4 GiB job. An instance is priced as a small managed instance for the same reason.
- **`model_tokens` adds input and output together**, so its price assumes 10 input tokens to each
  output token on the default model. Output-heavy use costs Orama more than it bills.

### Plans

| | Free (`free`) | Team (`team`) | Business (`business`) |
|---|---|---|---|
| Monthly fee | $0 | $149 | $799 |
| `storage` | 5 GiB | 100 GiB | 1 TiB |
| `transfer` | 10 GiB | 250 GiB | 1 TiB |
| `build_compute` | 10 hours | 50 hours | 300 hours |
| `run_compute` | 10 hours | 100 hours | 500 hours |
| `speech` | 30 minutes | 600 minutes | 3,000 minutes |
| `calls` | 1 million | 10 million | 50 million |
| `predictions` | 10,000 | 1 million | 10 million |
| `model_tokens` | 250,000 | 2 million | 10 million |
| `instances` | 750 hours | 1,500 hours | 5,000 hours |
| `volumes` | 10 GiB-months | 100 GiB-months | 1,000 GiB-months |
| Beyond the included amounts | list | list | Business overage |

Every workspace's own engine is an instance, so Free's 750 instance-hours cover one workspace all
month, and Team's 1,500 a workspace and one service. Free needs no card. Its usage beyond the
included amounts is billed at list; nothing stops it yet (usage limits warn and do not refuse,
[metering.md](metering.md#limits)). Business adds priority support, which lake does not model.

Each plan's fee is set so that its included amounts, fully used, cost about the fee at the
reference rates (Team about $130, Business about $615), and most accounts use less. The plan shape
follows hosted git and ML platforms: a free tier with small included amounts, then flat monthly
plans with included usage and metered overage. Lake has no seat dimension, so plans are not priced
per seat.

### Worked bills

A 31-day month, computed as lake computes a bill (⌈billable × `unit_micros` ÷ `per`⌉ per line):

| Example | Usage | Bill |
|---|---|---|
| Small | one workspace and one Postgres all month (1,488 instance-hours); 40 GiB stored, 120 GiB transfer, 30 build hours, 40 run hours, 200 speech minutes, 3 million calls, 200,000 predictions, 3 million tokens, 30 GiB-months of volumes | Team **$158.00**: $149 and 1 million tokens over ($9.00) |
| Medium | five instances all month (3,720 hours); 300 GiB, 800 GiB, 120 and 300 hours, 1,500 minutes, 25 million calls, 3 million predictions, 15 million tokens, 250 GiB-months | Business **$844.00**: $799 and 5 million tokens over ($45.00). On Team: $634.58 (instances $177.60, tokens $117.00, run $48.00, calls $30.00, volumes $30.00, transfer $27.50, build $21.00, predictions $20.00, storage $8.00, speech $6.48) |
| Large | twelve instances all month (8,928 hours); 3 TiB, 4 TiB, 600 and 2,000 hours, 10,000 minutes, 200 million calls, 50 million predictions, 80 million tokens, 2,000 GiB-months | Business **$2,996.04**: $799, tokens $630.00, predictions $320.00, run $300.00, instances $255.32, calls $225.00, volumes $160.00, transfer $122.88, build $72.00, storage $61.44, speech $50.40. On Team: $3,695.10 |

### Revisit

Revisit the price list every quarter, and sooner when:

- a reference rate moves more than 15%, above all the model provider's rate for the default agent
  model, which `model_tokens` passes through;
- lake's agents change their default model;
- the operator reports CPU and memory (lake#612), so compute can be priced per vCPU-hour and
  GiB-hour;
- `model_tokens` is split into input and output, or into models, so each can carry its own rate.

A change is a new `set_billing_plan` for each plan. It applies to bills worked out from then on,
including the current month's; issued invoices keep their lines.

### Publish the price list

As the operator, once per plan (amounts in millionths of a dollar, `included` in each dimension's
unit):

```http
POST /v1/set_billing_plan
{"id":"free","name":"Free","base_micros":0,"currency":"USD","active":true,"position":10,"prices":[{"dimension":"storage","unit_micros":40000,"per":1073741824,"included":5368709120},{"dimension":"transfer","unit_micros":50000,"per":1073741824,"included":10737418240},{"dimension":"build_compute","unit_micros":300000,"per":3600,"included":36000},{"dimension":"run_compute","unit_micros":240000,"per":3600,"included":36000},{"dimension":"speech","unit_micros":7200,"per":60,"included":1800},{"dimension":"calls","unit_micros":2000000,"per":1000000,"included":1000000},{"dimension":"predictions","unit_micros":10000,"per":1000,"included":10000},{"dimension":"model_tokens","unit_micros":9000000,"per":1000000,"included":250000},{"dimension":"instances","unit_micros":80000,"per":3600,"included":2700000},{"dimension":"volumes","unit_micros":200000,"per":2628000,"included":26280000}]}
```

```http
POST /v1/set_billing_plan
{"id":"team","name":"Team","base_micros":149000000,"currency":"USD","active":true,"position":20,"prices":[{"dimension":"storage","unit_micros":40000,"per":1073741824,"included":107374182400},{"dimension":"transfer","unit_micros":50000,"per":1073741824,"included":268435456000},{"dimension":"build_compute","unit_micros":300000,"per":3600,"included":180000},{"dimension":"run_compute","unit_micros":240000,"per":3600,"included":360000},{"dimension":"speech","unit_micros":7200,"per":60,"included":36000},{"dimension":"calls","unit_micros":2000000,"per":1000000,"included":10000000},{"dimension":"predictions","unit_micros":10000,"per":1000,"included":1000000},{"dimension":"model_tokens","unit_micros":9000000,"per":1000000,"included":2000000},{"dimension":"instances","unit_micros":80000,"per":3600,"included":5400000},{"dimension":"volumes","unit_micros":200000,"per":2628000,"included":262800000}]}
```

```http
POST /v1/set_billing_plan
{"id":"business","name":"Business","base_micros":799000000,"currency":"USD","active":true,"position":30,"prices":[{"dimension":"storage","unit_micros":30000,"per":1073741824,"included":1099511627776},{"dimension":"transfer","unit_micros":40000,"per":1073741824,"included":1099511627776},{"dimension":"build_compute","unit_micros":240000,"per":3600,"included":1080000},{"dimension":"run_compute","unit_micros":200000,"per":3600,"included":1800000},{"dimension":"speech","unit_micros":7200,"per":60,"included":180000},{"dimension":"calls","unit_micros":1500000,"per":1000000,"included":50000000},{"dimension":"predictions","unit_micros":8000,"per":1000,"included":10000000},{"dimension":"model_tokens","unit_micros":9000000,"per":1000000,"included":10000000},{"dimension":"instances","unit_micros":65000,"per":3600,"included":18000000},{"dimension":"volumes","unit_micros":160000,"per":2628000,"included":2628000000}]}
```

Each payload also carries the plan's `description` (in lake#623). Then `GET /v1/billing_plans`
lists the three, and `GET /v1/bill_preview?customer_id=<id>` prices an account's month on its plan.

## Capabilities

| Capability | Kind | Who | Input | What it does |
|---|---|---|---|---|
| `billing_plans` | read | anyone signed in | `include_retired?` | The price list: active plans by `position` then `id`, each with its prices (each with the dimension's `title` and `unit`). Retired plans only with `include_retired` and the operator. |
| `set_billing_plan` | action | operator, instance administrator | `id`, `name`, `base_micros`, `description?`, `currency?` (default `USD`), `active?`, `position?`, `prices?` | Creates or replaces one plan and all its prices. `id` is `^[a-z][a-z0-9-]{0,62}$` and never changes; `active: false` retires the plan (no account can choose it; accounts on it stay). A dimension priced twice is `invalid_input`. Audit object `billing_plan/<id>`. |
| `billing_account` | read | `read` on the customer | `customer_id` | The plan this month (`current`), the plan from next month (`next`, when it changes), every change (`history`) and the payment `provider`. |
| `choose_billing_plan` | action | `admin` on the customer | `customer_id`, `plan_id` | Chooses the account's plan as above. A retired plan is `invalid_input`, an unknown one `not_found`. Answers as `billing_account`. |
| `bill_preview` | read | `read` on the customer | `customer_id`, `period?` (`YYYY-MM`; default this month) | The bill for the month worked out now; nothing is stored. `complete` is false when a workspace's usage could not be read (`unread_workspaces`); `not_measured` repeats what `usage_over_time` cannot include; `invoice` is the month's issued invoice that is not void, or null. No plan for the month is `conflict`. |
| `issue_invoice` | action | operator, instance administrator | `customer_id`, `period` | Issues the invoice for a month that has ended (`conflict` before), on the plan of that month, with the lines frozen, then asks the provider for payment. Refused with `store_unavailable` while any workspace's usage cannot be read, never issued short; `conflict` when the month already has an invoice that is not void. Issued by hand today; automatic monthly issue is [lake#621](https://github.com/oramasearch/lake/issues/621). |
| `list_invoices` | read | `read` on the customer | `customer_id` | The account's invoices, newest month first, each with its lines. |
| `get_invoice` | read | `read` on the customer | `customer_id`, `invoice_id` | One invoice; `not_found` when it is not this account's. |
| `record_invoice_payment` | action | operator, instance administrator | `invoice_id`, `reference?`, `note?` | Marks a due invoice `paid` with the payment's reference. `conflict` when it is not due, or its provider reports payments itself (a Stripe invoice). |
| `void_invoice` | action | operator, instance administrator | `invoice_id`, `note` | Marks a due invoice `void`, after voiding it at its provider (Stripe); the month can be invoiced again. A paid invoice is never voided (`conflict`). |

The operator-only capabilities answer anyone else `unauthorized` before the handler runs. Audit
objects are `customer/<id>/billing` (kinds `billing.account.read`, `billing.plan.choose`,
`billing.bill.preview`, `billing.invoice.issue`, `billing.invoices.read`, `billing.invoice.read`,
`billing.invoice.paid`, `billing.invoice.void`) except `set_billing_plan`'s.

An invoice is `{id, number, customer_id, period, plan_id, plan_name, currency, total_micros, state,
provider, provider_reference, instructions, note, issued_at, paid_at, voided_at, payment_url,
provider_status, lines}`; `state` is
`due`, `paid` or `void`, and `number` increases across the instance. A line is `{kind, dimension,
description, unit, quantity, included, billable, unit_micros, per, amount_micros}`.

## Examples

The operator publishes a plan:

```http
POST /v1/set_billing_plan
{"id": "team", "name": "Team", "base_micros": 20000000, "currency": "USD", "prices": [{"dimension": "calls", "unit_micros": 1000000, "per": 1000, "included": 1000}, {"dimension": "storage", "unit_micros": 100000, "per": 1073741824, "included": 1073741824}]}
```

An account chooses it and reads its bill so far:

```http
POST /v1/choose_billing_plan
{"customer_id": "<customer id>", "plan_id": "team"}
```

- `GET /v1/bill_preview?customer_id=<customer id>`
- `GET /v1/bill_preview?customer_id=<customer id>&period=2026-09`

After the month ends the operator issues it, and records the payment:

```http
POST /v1/issue_invoice
{"customer_id": "<customer id>", "period": "2026-09"}
```

```http
POST /v1/record_invoice_payment
{"invoice_id": "<invoice id>", "reference": "WIRE-2026-10-001"}
```

## Verify

- `Billing.*` and `BillLines.*` (`tests/test_billing.cpp`): choosing a plan now and next month,
  cancelling a change, refusing a retired plan, an outsider and a person setting prices; a month's
  bill from two workspaces' calls, tokens and storage (averaged by day); invoices frozen, one a
  month, voided and reissued, paid by hand; a workspace that does not answer holding the invoice
  back; a fake provider through the interface; and the enterprise switch removing every billing
  capability, `viewer.billing` and the Billing guide while usage stays.
- `StripeSignature.*`, `StripeClient.*` and `StripeBilling.*` (`tests/test_stripe.cpp`, a fake
  Stripe): the signature with its tolerance window (bad, stale, future, several `v1`); forms and
  cents; a customer made once and kept; an invoice with its items, finalize and hosted URL, every
  write with its own idempotency key; a retry after a failed step that bills once; Stripe's total
  equal to lake's to the cent; a void in lake voiding in Stripe; webhooks applying paid, failed and
  void once per event id, refusing bad and stale signatures, acknowledging foreign and unknown
  events, with the central audit; the keys from the environment: unset, half set and malformed
  falling back to manual with a reason that names the variable in `billing_account`, `viewer` and
  `server_info`, never a value, and rotation by restart (lake#628); and the route answering only
  where the lake bills.
- `PostgresBilling.PlansSubscriptionsAndInvoicesFollowTheContract` and
  `PostgresBilling.PaymentProviderCustomersAndNoticesFollowTheContract` (`tests/test_postgres.cpp`):
  the migrations' tables and guards (`0073`, `0074`), and the store's contract on PostgreSQL.
- `SchemaConformance.EveryToolAndRequiredOutputUsesHttpContract` calls every billing tool over MCP
  and REST with billing on; `AuditRouting.EveryRegisteredCapabilityIsClassified` checks each is
  central.

## Change

- A new dimension priced: add it to `usage_dimensions()` ([metering.md](metering.md#dimensions));
  `set_billing_plan` accepts it at once.
- Another payment provider: implement `PaymentProvider` against the provider's HTTP API, choose it
  in `PaymentProviders` (by which secret the mount holds, say), record the choice as a decision, and
  document how its payment notices reach lake. Stripe's choice is decision 57; revisit it if fees,
  regions or an enterprise install's own processor call for another.
- A Stripe event lake should act on: map it in `parse_stripe_event` (`src/api/stripe.cpp`), add the
  transition to `apply_payment_event` in both stores, and register it in the Stripe dashboard.
