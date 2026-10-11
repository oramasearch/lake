# Billing: the price list, bills and invoices

Billing is Orama Cloud's (lake.live): one price list, the bill for a month worked out from an
account's metered usage at that list, and the invoices lake issues with their payment state
([lake#564](https://github.com/oramasearch/lake/issues/564)). It is **off on enterprise
installs**, where usage tracking and its charts stay on ([metering.md](metering.md#usage-over-time)).

## Usage only (lake#635)

[lake#635](https://github.com/oramasearch/lake/issues/635), Steve, 2026-10-11: *"pricing is usage
based only for lake... no free plan... just straight usage... metis is enterprise priced... talk to
sales"*.

- **One price list, one rate per usage dimension.** No plans, no Free, Team or Business tier, no
  monthly fee and no included amounts.
- **Every account is billed on it automatically.** There is nothing to choose: `choose_billing_plan`,
  `billing_plans`, `set_billing_plan` and the plan picker are gone; `price_list` and
  `set_price_list` replace them.
- **A bill is usage × rate**, per dimension, and an invoice freezes each line's quantity and rate.
- **Time is priced per minute** ([lake#637](https://github.com/oramasearch/lake/issues/637), Steve,
  2026-10-11: *"make the pricing per minute, not per hour"*): build and run compute, service
  instances and speech to text. Storage and volumes stay per GiB-month.
- **Metis is not on this list.** Metis is enterprise software, priced by Orama's sales team per
  customer; lake's price list prices Orama Cloud usage only.

## Where it lives (decision 56)

[Decision 56](https://github.com/oramasearch/liquid-bridge/blob/main/docs/decisions.md)
([lake#617](https://github.com/oramasearch/lake/issues/617)), Steve, 2026-10-10: *"billing records
go in postgres... usage records go in zetta workspace."*

- **Billing records** are multitenancy, in lake's control PostgreSQL (migration `0073_billing`,
  [schema](../control-plane/schema.md#billing-migration)): `billing_plans` and
  `billing_plan_prices` (each publication of the price list, [below](#how-the-list-is-stored)),
  `billing_invoices` (one account, one month, the price list it was billed at, a total and a payment
  state) and `billing_invoice_lines`. They hold account ids, dimensions, quantities and amounts,
  never workspace content. `billing_subscriptions` stays in the schema, empty and unused since
  lake#635: no migration was needed.
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
  price list, lines and total, and which issue of the month it is). It answers the provider's `reference`,
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
`metadata[lake_customer_id]`) the first time one of its invoices is issued, so accounts that are never
invoiced leave nothing in Stripe. Its id (`cus_...`) is kept in
`billing_accounts` (customer, provider, provider customer id), the account's billing row in
PostgreSQL (decision 56), and reused for every later invoice.

**Invoices.** `issue_invoice` freezes lake's invoice as before, then:

1. `POST /v1/invoices`: a draft for the customer, `currency=usd`, `collection_method=send_invoice`,
   `days_until_due=30`, `auto_advance=false`, `pending_invoice_items_behavior=exclude`, described
   as "Orama Cloud usage, `<month>`", with the month and the price list id
   (`metadata[lake_price_list_id]`) in its metadata;
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

The price list has at most one rate per usage dimension, keyed on the names `usage_over_time`
answers ([metering.md](metering.md#dimensions)): `unit_micros` for every `per` units of the
dimension's unit. Amounts are integer millionths of the list's three-letter `currency` (USD). A
dimension without a rate is not charged. There is no fee and nothing is included.

The bill for a month (`YYYY-MM`, a UTC calendar month) is one `usage` line per priced dimension, by
dimension name, with `quantity` (the month's use), `billable` (what is charged; `included` is always
0), the rate (`unit_micros`, `per`) and `amount_micros` = ⌈`billable` × `unit_micros` ÷ `per`⌉,
capped at the largest bigint. The total is the sum of the lines.

**Time is billed by the minute on the month's total** (lake#637, `billable_quantity` in
`include/lake/api/billing.hpp`). Usage is metered in seconds; for a dimension counted in `seconds`
or `instance_seconds` (`build_compute`, `run_compute`, `speech`, `instances`), `billable` is the
month's summed seconds rounded up once to a whole minute. A job is never rounded on its own: a
hundred one-second builds are 100 seconds, billed as 2 minutes ($0.01), not 100 minutes. Every
other dimension's `billable` is its `quantity`.

`quantity` for a counted dimension is the month's total across the account's live workspaces; for
storage (a reading) it is the average of the month's daily readings, each the sum of every
workspace's latest reading that day. Workspaces deleted before the bill is read are not included
(decision 56's cost).

### When prices change

**A month is billed at the list in force at its end.** Each publication applies from the first day
of the month it names (`effective_from`, this month by default) until the next one. Of the lists
published for a month or before, the one with the latest month wins, and of those the last
published. So:

- a list published on 20 October for October prices all of October, the days before it included;
- a list published in October for November leaves October alone and prices November onward
  (`price_list` shows it as `next`, and the Billing page announces it);
- an invoice already issued keeps the rates frozen on its lines; a new list for that month changes
  only its preview, and the month's next issue after a void.

Lake does not prorate by day: a whole month has one rate per dimension, which keeps a bill
explainable as usage × rate. To raise prices without touching a month already under way, publish
the new list for the next month.

### How the list is stored

No migration: each publication is one `billing_plans` row (name `Usage`, `base_micros` 0, every
price's `included` 0) with its rates in `billing_plan_prices`, under the id
`usage-<YYYYMM>-<publish time in µs, 17 digits>`. The id sorts by the month the list applies from,
then by when it was published, which is the rule above (`price_list_for`,
`include/lake/api/billing.hpp`). Every publication is kept, so an invoice's `price_list_id` always
names the rates it was billed at. Rows with any other id (plans written before lake#635) are
ignored.

## Price list

Orama Cloud's rates ([lake#623](https://github.com/oramasearch/lake/issues/623), Steve,
2026-10-11: *"come up with sensible prices based on market rates for similar services"*; usage only
since [lake#635](https://github.com/oramasearch/lake/issues/635)). Each rate is a public list price
for the same resource, read on 2026-10-11 in US regions, times a stated margin: about 1.5–3× for
infrastructure, for running it as a managed service, and the model provider's rate plus 20% for
model tokens and speech to text. Prices are in USD. lake#623 carries the full research table with
each reference's source URL.

**Metis is not on this list.** Metis is enterprise software priced by Orama's sales team; a
customer talks to sales. Nothing here prices it.

**Status:** not yet published on lake.oramadev.net (billing is not deployed there). `set_price_list`
needs the operator or the instance administrator; the payload is [below](#publish-the-price-list).

### Rates

`per` is what the rate counts, in the dimension's unit ([metering.md](metering.md#dimensions)).

| Dimension | Rate | `unit_micros` | `per` | Reference rate (2026-10-11) | Margin |
|---|---|---|---|---|---|
| `storage` | $0.04 / GiB-month | 40000 | 1073741824 bytes | hyperscaler object storage $0.023 / GB-month (the others $0.015–0.020) | 1.7× |
| `transfer` | $0.05 / GiB | 50000 | 1073741824 bytes | hyperscaler internet egress $0.087–0.12 / GB, ingress free | in and out are one dimension: ~1.1× a half-in, half-out blend, about half of egress alone |
| `build_compute` | $0.005 / minute ($0.30 / hour) | 5000 | 60 seconds | serverless containers $0.0405 / vCPU-hour + $0.00445 / GB-hour ($0.099 / hour, $0.00165 / minute, for 2 vCPU, 4 GiB); hosted CI runner, 2 cores, $0.36 / hour ($0.006 / minute) | 3× containers, below the CI runner |
| `run_compute` | $0.004 / minute ($0.24 / hour) | 4000 | 60 seconds | the same 2 vCPU, 4 GiB container, $0.099 / hour ($0.00165 / minute) | 2.4× |
| `speech` | $0.0072 / minute | 7200 | 60 seconds | the model provider's speech-to-text rate, $0.006 / minute | provider + 20% |
| `calls` | $2.00 / million | 2000000 | 1000000 calls | managed HTTP API gateway $1.00 / million (REST APIs $3.50) | 2× |
| `predictions` | $0.01 / 1,000 | 10000 | 1000 calls | serverless CPU inference, about $4.55 / million requests at 2 GB | 2.2× |
| `model_tokens` | $9.00 / million | 9000000 | 1000000 tokens | the model provider's per-token rate for lake's agents' default model: $5.00 / million input, $30.00 output; $7.27 / million at 10 input to 1 output | provider + 20%, rounded up |
| `instances` | $0.00135 / instance-minute ($0.081 / hour) | 1350 | 60 instance-seconds | a small managed database or broker instance (2 vCPU, 2 GiB), $0.032–0.046 / hour ($0.00053–0.00077 / minute) | ~2× ($0.08 / hour is $0.001333 / minute, rounded to $0.00135) |
| `volumes` | $0.20 / GiB-month | 200000 | 2628000 GiB-seconds (730 hours) | block volume $0.08 / GB-month | 2.5× |

Two rates rest on an assumption, because a dimension counts less than its reference does:

- **Compute is counted in seconds of a job, not vCPU-seconds** (the operator does not report CPU and
  memory, [lake#612](https://github.com/oramasearch/lake/issues/612)), so a build or run is priced
  as a 2 vCPU, 4 GiB job. An instance is priced as a small managed instance for the same reason.
- **`model_tokens` adds input and output together**, so its rate assumes 10 input tokens to each
  output token on the default model. Output-heavy use costs Orama more than it bills.

Every workspace's own engine is an instance, so a workspace that runs all month costs about $59 in
instance-minutes (43,800 × $0.00135) before anything else. There is no free allowance: usage limits warn
and do not refuse ([metering.md](metering.md#limits)).

### Worked bills

A 31-day month at the list rates, computed as lake computes a bill (⌈billable × `unit_micros` ÷
`per`⌉ per line; the hours below are whole minutes, so nothing rounds):

| Example | Usage | Bill |
|---|---|---|
| Small | one workspace and one Postgres all month (1,488 instance-hours); 40 GiB stored, 120 GiB transfer, 30 build hours, 40 run hours, 200 speech minutes, 3 million calls, 200,000 predictions, 3 million tokens, 30 GiB-months of volumes | **$189.17**: instances $120.53, tokens $27.00, run $9.60, build $9.00, transfer $6.00, calls $6.00, volumes $6.00, predictions $2.00, storage $1.60, speech $1.44 |
| Medium | five instances all month (3,720 hours); 300 GiB, 800 GiB, 120 and 300 hours, 1,500 minutes, 25 million calls, 3 million predictions, 15 million tokens, 250 GiB-months | **$737.12**: instances $301.32, tokens $135.00, run $72.00, calls $50.00, volumes $50.00, transfer $40.00, build $36.00, predictions $30.00, storage $12.00, speech $10.80 |
| Large | twelve instances all month (8,928 hours); 3 TiB, 4 TiB, 600 and 2,000 hours, 10,000 minutes, 200 million calls, 50 million predictions, 80 million tokens, 2,000 GiB-months | **$3,802.85**: instances $723.17, tokens $720.00, predictions $500.00, run $480.00, calls $400.00, volumes $400.00, transfer $204.80, build $180.00, storage $122.88, speech $72.00 |

### Revisit

Revisit the price list every quarter, and sooner when:

- a reference rate moves more than 15%, above all the model provider's rate for the default agent
  model, which `model_tokens` passes through;
- lake's agents change their default model;
- the operator reports CPU and memory (lake#612), so compute can be priced per vCPU-hour and
  GiB-hour;
- `model_tokens` is split into input and output, or into models, so each can carry its own rate.

A change is a new `set_price_list`, normally for the next month ([When prices
change](#when-prices-change)). Issued invoices keep their lines.

### Publish the price list

As the operator, once (amounts in millionths of a dollar, `per` in each dimension's unit; time per
60 seconds, a minute):

```http
POST /v1/set_price_list
{"currency":"USD","prices":[{"dimension":"storage","unit_micros":40000,"per":1073741824},{"dimension":"transfer","unit_micros":50000,"per":1073741824},{"dimension":"build_compute","unit_micros":5000,"per":60},{"dimension":"run_compute","unit_micros":4000,"per":60},{"dimension":"speech","unit_micros":7200,"per":60},{"dimension":"calls","unit_micros":2000000,"per":1000000},{"dimension":"predictions","unit_micros":10000,"per":1000},{"dimension":"model_tokens","unit_micros":9000000,"per":1000000},{"dimension":"instances","unit_micros":1350,"per":60},{"dimension":"volumes","unit_micros":200000,"per":2628000}]}
```

It applies from the start of this month; add `"effective_from": "YYYY-MM"` for another month. Then
`GET /v1/price_list` answers it, and `GET /v1/bill_preview?customer_id=<id>` prices an account's
month at it. Administration → Billing (`/billing/plans`) is the same publication from the web app,
pre-filled with these rates.

## Capabilities

| Capability | Kind | Who | Input | What it does |
|---|---|---|---|---|
| `price_list` | read | anyone signed in | `period?` (`YYYY-MM`; default this month), `include_history?` | `{period, list, next, history?}`: the list in force for the month (null before one is published), a list published for a later month (or null), and, for the operator with `include_history`, every publication, newest first. A list is `{id, effective_from, published_at, currency, prices}`, each price `{dimension, title, unit, unit_micros, per}`. |
| `set_price_list` | action | operator, instance administrator | `prices`, `currency?` (default `USD`), `effective_from?` (`YYYY-MM`; default this month) | Publishes the price list, kept as a new publication. A dimension rated twice, an unknown dimension, `included` or a fee is `invalid_input`. Audit object `billing_price_list/<id>`, kind `billing.price_list.set`. |
| `billing_account` | read | `read` on the customer | `customer_id` | `{customer_id, month, price_list, provider}`: the list the account is billed at this month (null before one is published) and the payment `provider`. |
| `bill_preview` | read | `read` on the customer | `customer_id`, `period?` (`YYYY-MM`; default this month) | The bill for the month worked out now at the list in force for it; nothing is stored. `price_list` is `{id, effective_from}`. `complete` is false when a workspace's usage could not be read (`unread_workspaces`); `not_measured` repeats what `usage_over_time` cannot include; `invoice` is the month's issued invoice that is not void, or null. No list for the month is `conflict`. |
| `issue_invoice` | action | operator, instance administrator | `customer_id`, `period` | Issues the invoice for a month that has ended (`conflict` before), at the list in force for it, with the lines frozen, then asks the provider for payment. Refused with `store_unavailable` while any workspace's usage cannot be read, never issued short; `conflict` when the month already has an invoice that is not void. Issued by hand today; automatic monthly issue is [lake#621](https://github.com/oramasearch/lake/issues/621). |
| `list_invoices` | read | `read` on the customer | `customer_id` | The account's invoices, newest month first, each with its lines. |
| `get_invoice` | read | `read` on the customer | `customer_id`, `invoice_id` | One invoice; `not_found` when it is not this account's. |
| `record_invoice_payment` | action | operator, instance administrator | `invoice_id`, `reference?`, `note?` | Marks a due invoice `paid` with the payment's reference. `conflict` when it is not due, or its provider reports payments itself (a Stripe invoice). |
| `void_invoice` | action | operator, instance administrator | `invoice_id`, `note` | Marks a due invoice `void`, after voiding it at its provider (Stripe); the month can be invoiced again. A paid invoice is never voided (`conflict`). |

The operator-only capabilities answer anyone else `unauthorized` before the handler runs. Audit
objects are `customer/<id>/billing` (kinds `billing.account.read`, `billing.bill.preview`,
`billing.invoice.issue`, `billing.invoices.read`, `billing.invoice.read`, `billing.invoice.paid`,
`billing.invoice.void`) except `set_price_list`'s.

An invoice is `{id, number, customer_id, period, price_list_id, currency, total_micros, state,
provider, provider_reference, instructions, note, issued_at, paid_at, voided_at, payment_url,
provider_status, lines}`; `state` is `due`, `paid` or `void`, and `number` increases across the
instance. A line is `{kind, dimension, description, unit, quantity, included, billable, unit_micros,
per, amount_micros}`, with `kind` `usage`, `included` 0, and `billable` the `quantity` charged
(time rounded up to a whole minute on the month's total).

## Examples

The operator publishes a list for November, a month ahead:

```http
POST /v1/set_price_list
{"effective_from": "2026-11", "prices": [{"dimension": "calls", "unit_micros": 2000000, "per": 1000000}, {"dimension": "storage", "unit_micros": 40000, "per": 1073741824}]}
```

An account reads the rates and its bill so far, with nothing to choose:

- `GET /v1/price_list`
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

- `Billing.*` and `BillLines.*` (`tests/test_billing.cpp`): every unit billed at the list rate,
  rounded up, with no fee line; the list in force at a month's end (a list for the month published
  later wins, a list for a later month waits, plans from before lake#635 are ignored); publishing
  (operator only, one rate a dimension, no fee or included amount, `next` and history); time
  billed by the minute on the month's summed seconds, rounded up once (lake#637); every
  account billed at it with nothing to choose, the retired plan capabilities absent, and an
  outsider refused; a month's bill from two workspaces' calls, tokens and storage (averaged by
  day); invoices frozen against later usage and a later list for the month, one a month, voided
  and reissued, paid by hand; a workspace that does not answer holding the invoice
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
- `PostgresBilling.PriceListsAndInvoicesFollowTheContract` and
  `PostgresBilling.PaymentProviderCustomersAndNoticesFollowTheContract` (`tests/test_postgres.cpp`):
  the migrations' tables and guards (`0073`, `0074`), and the store's contract on PostgreSQL.
- `SchemaConformance.EveryToolAndRequiredOutputUsesHttpContract` calls every billing tool over MCP
  and REST with billing on; `AuditRouting.EveryRegisteredCapabilityIsClassified` checks each is
  central.

## Change

- A new dimension priced: add it to `usage_dimensions()` ([metering.md](metering.md#dimensions)) and
  to `DIMENSIONS` and `LIST_RATES` in `frontend/src/pages/billing/billing.ts`; `set_price_list`
  accepts it at once.
- New rates: publish a new list, normally for the next month, and update the [rates](#rates) table
  and its references here.
- Another payment provider: implement `PaymentProvider` against the provider's HTTP API, choose it
  in `PaymentProviders` (by which secret the mount holds, say), record the choice as a decision, and
  document how its payment notices reach lake. Stripe's choice is decision 57; revisit it if fees,
  regions or an enterprise install's own processor call for another.
- A Stripe event lake should act on: map it in `parse_stripe_event` (`src/api/stripe.cpp`), add the
  transition to `apply_payment_event` in both stores, and register it in the Stripe dashboard.
