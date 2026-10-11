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

Lake ships two implementations. **Stripe** ([below](#stripe)) where its keys are in lake's secret
store, and **manual** ("Invoice only") everywhere else: lake issues the invoice, Orama collects
payment outside lake, and the operator records it. A provider is lake's own code against the
provider's HTTP API through lake's outbound client, never a third-party SDK (liquid-bridge
`CLAUDE.md`, "We write our own"). Tests inject a fake through `register_billing_capabilities` or
`ServerOptions::payment_providers`.

Which provider is in use is decided at use, every time (`PaymentProviders`): the billing
capabilities and the notice route ask for the current one, so keys put, rotated or removed while
lake runs take effect without a restart. `record_invoice_payment` goes by the invoice's own
provider: an invoice issued by the manual provider stays recordable after Stripe is set up, and a
Stripe invoice never is (`conflict`).

`billing_account` answers `provider: {name, title, records_payments_by_hand, online_payment,
reason}`; `reason` says why card payment is not available while the manual provider is in use.
`viewer.billing_payment` (`{provider, title, online_payment, reason, checked_at}`, null where billing
is off) and `server_info` (`billing`, `payment_provider`, `online_payment`) report the same, from
the last time lake read the keys.

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

**Lake's own secret store holds the Stripe keys, not a Kubernetes Secret** (decision 57). Secrets
live in a workspace Zetta's sealed secret mount ([secrets.md](secrets.md)) and become a Kubernetes
Secret only when a deployment, app or service has to mount one; lake reads its own keys from the
store.

- `LAKE_BILLING_SECRETS` is the **id of a secret mount**, a mount owned by the operator's account
  in the instance's operator workspace. It is the only configuration in the environment; no key is
  ever in `spec.env`, a Secret or a log.
- In that mount, the secret **`stripe`** holds `{"secret_key": "sk_...", "webhook_secret":
  "whsec_..."}` (a restricted `rk_...` key works if it may write customers, invoices and invoice
  items).
- Lake reads it with `read_secret` as the operator at use, keeps it in memory for 60 seconds, and
  reads it again after that, or at once when a notice's signature fails. Putting a new version
  rotates the keys within a minute, and the webhook secret at the first notice. Each read is in the
  mount's workspace audit.
- **Fallback.** Billing on with `LAKE_BILLING_SECRETS` unset, a mount that does not exist, no
  `stripe` secret, a missing field or a key of the wrong shape: the manual provider, with the reason
  on the Billing page (the operator sees the reason; an account sees "Card payment is not set up"),
  in `billing_account`, `viewer.billing_payment` and `server_info`. If the mount's workspace does
  not answer, lake keeps the keys it read last; with none, the call answers `store_unavailable`
  rather than issuing a manual invoice by mistake.
- **Billing off** (an enterprise install): no provider, no route, nothing read.

Setting it up (the operator, with the operator's bearer):

```http
POST /v1/create_secret_mount
{"customer_id": "<operator account id>", "workspace_id": "<operator workspace id>", "name": "billing"}
```

```http
POST /v1/put_secret
{"mount_id": "<mount id>", "name": "stripe", "value": {"secret_key": "sk_test_...", "webhook_secret": "whsec_..."}}
```

Then set `LAKE_BILLING_SECRETS=<mount id>` in the Lake CR's `spec.env`
([aks.md](../deployment/aks.md#stripe)). The Secrets pages do the same: Administration → Secrets
(`/secret-mounts`), the mount, **New secret**, name `stripe`, with the two fields.

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
  events, with the central audit; the keys read from a secret mount, cached, rotated (a new webhook
  secret accepted at once) and removed, falling back to manual with a reason in `billing_account`,
  `viewer` and `server_info`; and the route answering only where the lake bills.
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
