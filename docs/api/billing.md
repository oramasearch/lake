# Billing: the price list, bills and invoices

Billing is Orama Cloud's (lake.live): one price list, the bill for a month worked out from an
account's metered usage at that list, and the invoices lake issues with their payment state
([lake#564](https://github.com/oramasearch/lake/issues/564)). It is **off on enterprise
installs**, where usage tracking and its charts stay on ([metering.md](metering.md#usage-over-time)).

## Usage only (lake#635)

[lake#635](https://github.com/oramasearch/lake/issues/635), Steve, 2026-10-11: *"pricing is usage
based only for lake... no free plan... just straight usage... metis is enterprise priced... talk to
sales"*.

- **One price list**, with rates per usage dimension, GPU class and model input/output class.
  No plans, no Free, Team or Business tier, no monthly fee and no included amounts.
- **Every account is billed on it automatically.** There is nothing to choose: `choose_billing_plan`,
  `billing_plans`, `set_billing_plan` and the plan picker are gone; `price_list` and
  `set_price_list` replace them.
- **A bill is usage × rate**, per dimension, and an invoice freezes each line's quantity and rate.
- **Time is priced per minute** ([lake#637](https://github.com/oramasearch/lake/issues/637), Steve,
  2026-10-11: *"make the pricing per minute, not per hour"*): compute and speech to text. Storage
  and volumes stay per GiB-month.
- **Compute is CPU, RAM, GPU and GPU memory, each at its own rate**
  ([lake#643](https://github.com/oramasearch/lake/issues/643), Steve, 2026-10-11: *"service minutes
  are actually cpu minutes and ram minutes (different prices)... and gpu minutes and vram
  minutes"*): vCPU-minutes, GiB-minutes of RAM, GPU-minutes (a rate per GPU class) and GiB-minutes
  of GPU memory, of the resources every workload is scheduled with
  ([metering.md](metering.md#resources)). They replace instance-minutes and build and run
  job-minutes on the price list.
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
- **A deleted workspace's unbilled usage** is the one other exception: it is kept centrally, as
  quantities only, from the moment the workspace is deleted ([below](#a-deleted-workspaces-usage)).

## A deleted workspace's usage

[LSR-121](https://github.com/oramasearch/lake/issues/988): deleting a workspace removes its Zetta,
and with it the usage a bill reads. Before, `usage_over_time` read only live workspaces, so an
account that deleted a workspace before its month was invoiced was never billed for what it had
used, and its model tokens stopped counting against its monthly limit.

**What lake keeps** (migration `0108_retained_usage`, guard `retained_usage_guard`, billing on
only): before the cascade, `delete_workspace` flushes the usage recorder, reads every usage dimension
of the workspace through `usage_over_time`'s reader, a point a day, from the first day no issued
invoice covers (the month after the account's latest invoice that is not void; else the month the
workspace was created; at most 400 days back, the series' retention) through today, and writes it
in one transaction as `retained_usage` rows (`source = 'rollup'`) with one `retained_usage_sets` row
naming the window. Each row holds a dimension, the labels billing and the breakdowns read (`kind`,
`class`, `model`; never an object id), a bucket start and a quantity: content-free, multitenancy
like the invoice lines (decision 56).

**Fail closed.** If the workspace's usage cannot be read in full (any reader answers `measured:
false`), or the rows cannot be written, `delete_workspace` answers `store_unavailable` and deletes
nothing; retry once the workspace Zetta answers. The synchronous handler, which skips the read,
refuses a live workspace's deletion with `conflict` where billing is on, so no path deletes a
workspace without retaining its usage first. With billing off (`LAKE_BILLING`, an enterprise
install) nothing is retained and deletion is unchanged.

**What reads it.** Whenever no workspace is chosen, `usage_over_time` adds each deleted workspace's
retained rows as if read from its Zetta, so `bill_preview`, `issue_invoice`, the balance's refresh,
`usage_limits` and the model gateway's monthly `model_tokens` limit all count it. The deleted
workspace is listed in the reading's `workspaces` with `deleted: true`; when its rows cannot be
read it is `measured: false`, and the bill waits, as for any unread workspace. A workspace deleted
within the window that has no rows (deleted before migration 0108, or while billing was off) is
named in `not_measured`.

**Values still in flight.** A counter value the usage recorder held when the workspace was deleted
is added to its rows (`source = 'late'`, one row an hour) at the next flush instead of being
dropped. A repeated `delete_workspace` of a live workspace (a retry after a refusal) writes the
rollup again and replaces the first; `late` rows are kept. A deleted workspace's deletion that the
owner loop resumes needs no read: its usage was retained before its cascade committed.

**Verify.** `Billing.ADeletedWorkspacesUsageStaysOnTheUnissuedBill` (`tests/test_billing.cpp`) deletes a
workspace that has used calls and checks this month's preview is unchanged, a later recorder value
is added, a repeated delete changes nothing, and a deletion whose usage cannot be read is refused;
`RetainedUsage.ARollupIsReplacedAndLateValuesAddUp` covers the store. On a Lake:

```sh
psql "$LAKE_CONTROL_DB_URI" -c "SELECT workspace_id, from_at, to_at, retained_at FROM retained_usage_sets"
```

**Operator.** The operator must vendor the schema and select `retained_usage_guard` before the image
rolls; without the tables a Lake with billing on refuses every workspace deletion.

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
- `provider_customer(account)`: the provider's id for the account, created the first time with the
  account's [billing contact](#billing-contact) when it has one, and the kept id it `replaces` when
  the provider no longer has that customer ([below](#stripe), lake#856); lake keeps it
  (`billing_accounts`) before it asks for payment, so a failed request never loses it;
- `update_customer(account)`: sends a saved billing contact to the provider's customer the account
  already has; an error leaves the contact lake keeps as it is;
- `request_payment(account, invoice)`: called for an invoice about to be issued (its account, month,
  price list, lines and total, and which issue of the month it is). It answers the provider's `reference`,
  the `instructions` the invoice shows, the `payment_url` where the account pays and the provider's
  `provider_status`, or an error, which leaves the invoice unissued (`store_unavailable`);
- `send_payment_request(account, invoice, resend)`: called once lake has stored an issued invoice,
  when the account has a billing contact and the invoice a `provider_reference`, and by
  `resend_invoice`; it sends the invoice to the billing email and answers the provider's status
  after it, or an error, which leaves the invoice issued ([Emailing invoices](#emailing-invoices)).
  The default sends nothing;
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

**One account, several apps** (lake#855). Lake bills through the same Stripe account as Orama
Cloud v1, and two Lakes can share it (orama-bridge#199). Every object lake creates there, a
customer, an invoice or an order sheet Checkout Session and its subscription
([orders.md](orders.md#paying)), carries `metadata[orama_app]=lake` and
`metadata[lake_instance]`, the Lake's public host (`LAKE_PUBLIC_HOST`; left out when it is unset),
beside lake's own `lake_*` keys. Lake never sets Cloud v1's bare keys `team_id`, `plan_id`,
`addon_type` or `addon_config`: Cloud v1's webhook handlers read them, so a Lake object carrying
one could be taken for Cloud v1's. A Customer is per app: a person who uses both products has two,
and neither app reuses the other's `cus_` id. Events about objects that are not this Lake's are
acknowledged and ignored ([below](#webhooks)).

**Products** (lake#855). Every line lake sends Stripe names one of Lake's products with an inline
price (`price_data[product]`), so Stripe's reports tell Lake's revenue from Cloud v1's:

| Product id | Lines |
|---|---|
| `lake_usage` | every line of a monthly invoice |
| `lake_private_warehouse` | the order sheet's private warehouse subscription ([orders.md](orders.md#paying)) |
| `lake_dedicated_cluster` | the order sheet's dedicated cluster subscription |

The ids are fixed and live in one place, `include/lake/api/stripe.hpp`. Lake never creates the
products: the operator does, in every account Lake bills through (the Lake's `spec.payments`,
orama-operator `docs/lake/payments.md`). Prices stay lake's own, the price list and the order
sheet; no Stripe Price is created by hand. A request naming a product the account lacks fails with
"Stripe has no product `<id>` in this account; the operator creates it (spec.payments)". A Lake on
this version must have its products before it bills; one on the manual provider is unaffected.

**Customers.** A lake account becomes one Stripe Customer (`POST /v1/customers`, name and
`metadata[lake_customer_id]`, with the tags above) the first time one of its invoices is issued, so
accounts that are never invoiced leave nothing in Stripe. It carries the account's billing contact,
its email, name and address, when one is saved ([below](#billing-contact)); without one, the name is
the account's. Its id (`cus_...`) is kept in `billing_accounts` (customer, provider, provider
customer id), the account's billing row in PostgreSQL (decision 56), and reused for every later
invoice.

**A customer Stripe no longer has** (lake#856). A `cus_` id exists in one Stripe account and mode
only. Before each invoice lake reads the kept customer (`GET /v1/customers/<id>`). When Stripe
answers 404 `resource_missing` (the Lake's key now names another sandbox, or live mode instead of
test mode) or the customer is `deleted: true` (deleted in the Dashboard), lake creates a new
customer as for the first invoice and keeps it in place of the old one, only while the row still
holds the old id: of two issues replacing it at once, both use the replacement stored first. The
central audit records `billing.account.provider_customer_replaced` with the old and new ids. Any
other failure of the read (401, 403, 429, 5xx, no answer) fails the issue and keeps the customer.
Invoices issued to the old customer keep their `provider_reference` and `payment_url`; they belong
to the old account.

Before switching a Lake to another Stripe account or mode, settle or void its open Stripe invoices.
Lake cannot learn about a payment made in the old account afterwards. New invoices go to a new
customer.

**Invoices.** `issue_invoice` freezes lake's invoice as before, then:

1. `POST /v1/invoices`: a draft for the customer, `currency=usd`, `collection_method=send_invoice`,
   `days_until_due=30`, `auto_advance=false`, `pending_invoice_items_behavior=exclude`, described
   as "Lake usage, `<month>`", with the month and the price list id
   (`metadata[lake_price_list_id]`) in its metadata, beside `lake_customer_id` and the tags above;
2. `POST /v1/invoiceitems`: one item per lake line with an amount, in cents (millionths rounded half
   up), described as the line is and tagged as above, of the `lake_usage` product at an inline
   price: `quantity=1`, `price_data[currency]=usd`, `price_data[product]=lake_usage`,
   `price_data[unit_amount]=<cents>`. When the rounded lines do not add up to lake's total rounded
   to the cent, one more item, **Rounding to the cent**, carries the difference, of the product too
   when it is positive. A negative one keeps the bare form (`currency=usd`, `amount=<cents>`),
   because an inline price cannot be negative;
3. `POST /v1/invoices/<id>/finalize`, whose answer gives `hosted_invoice_url` and `status`.

Lake keeps the Stripe invoice id as `provider_reference`, the hosted page as `payment_url` and
Stripe's status as `provider_status` (`open`). **Lake's invoice stays the source of truth for
amounts**: Stripe's total is lake's to the cent, and the lines are lake's. Stripe bills USD only
here; another currency is refused before anything is sent.

Every write is form-encoded with `Authorization: Bearer <secret key>`, `Stripe-Version:
2024-06-20` (`stripe_api_version`) and an `Idempotency-Key`; the read of a kept customer is a `GET`
with the same secret key and version and no `Idempotency-Key`. The idempotency keys are
`lake-customer-<customer>` for the customer, `lake-customer-<customer>-replaces-<old cus_ id>` for
one that replaces a customer Stripe no longer has (the plain key would replay the first customer
within Stripe's 24 hours in the same account), `lake-invoice-<customer>-<month>-<n>` for the
invoice (n is the issue of the month: 0,
then 1 after a void), with `-line-<i>-p`, `-rounding` and `-finalize` for the steps after it and
`-send` for the [email](#emailing-invoices) (`-send-<uuid>` for each resend), and
`lake-void-<invoice>` for a void. A retry of a failed issue within Stripe's 24 hours therefore gets
Stripe's first answers back instead of a second invoice; an issue after a void is a new invoice.
Stripe refuses a key sent again with other parameters, so a retry of an issue that failed under an
older Lake whose request was different is refused with Stripe's idempotency error until the key
expires, 24 hours after the first attempt; issue it again after that. Nothing is billed twice.
lake#855 changed the customer's and the invoice's fields (the tags, and the invoice's description
"Lake usage") under the same keys, so this applies to an issue that failed just before the
upgrade; no Lake billed through Stripe before it. Its lines' keys gained `-p` with their product.
Stripe's error message is kept, except on 401 and 403, where it can echo part of the key.

**A missing product.** When Stripe answers `resource_missing` on `price_data[product]` (the
operator has not created `lake_usage` in this account yet), `issue_invoice` fails
(`store_unavailable`) with "Stripe has no product lake_usage in this account; the operator creates
it (spec.payments)". Nothing is issued in lake, and the Stripe draft stays a draft. Once the
operator's `spec.payments` has created the product, the operator issues the invoice again: the same
month and attempt send the same keys, so the draft is reused. Stripe may have saved the refusal
under the item's key and replay it for 24 hours; if the same message comes back after the product
exists, issue again after 24 hours.

**Payment.** The account pays on Stripe's hosted invoice page, by card or bank, from the link in
Stripe's email ([below](#emailing-invoices)) or **Pay with Stripe** on lake's invoice page. Stripe
tells lake through its webhook ([below](#webhooks)).

**Voids.** `void_invoice` on a due Stripe invoice voids it in Stripe first (`POST
/v1/invoices/<id>/void`); if Stripe refuses (it was paid meanwhile), lake's invoice stays due. A
Stripe invoice cannot be voided while lake has no Stripe keys (`store_unavailable`): void it in
Stripe, and Stripe's notice voids it in lake.

The client is `src/api/stripe.cpp` (`include/lake/api/stripe.hpp`); its HTTP exchange is the
server's outbound client, injected as a `StripeHttp` the tests replace with a fake Stripe.

## Billing contact

[lake#857](https://github.com/oramasearch/lake/issues/857): where an account's invoices go, and the
name and address a business needs on them. It is a billing record, one row per account in
`billing_contacts` (migration `0090_billing_contacts`, guard `billing_contacts_guard`), in lake's
control PostgreSQL (decision 56), deleted with the account:

| Field | Holds | At most |
|---|---|---|
| `email` | where invoices go; required, one `@` with something on each side and no whitespace | 320 characters |
| `name` | the legal or company name on the invoice; empty: the account's name is used | 256 |
| `line1`, `line2` | the street address | 256 each |
| `city`, `state` | | 128 each |
| `postal_code` | | 32 |
| `country` | an ISO 3166-1 alpha-2 code in capitals (`DE`), or empty | 2 |

Lengths are characters, as PostgreSQL counts them. The table's checks and `billing_contact_problem`
(`include/lake/control/store.hpp`), which both stores and `set_billing_contact` apply, say the same.

**Reading it.** `billing_account` answers `contact`, `{email, name, line1, line2, city, postal_code,
state, country, updated_at}`, or null while the account has none. While it has none,
`suggested_email` is the signed-in person's own email (from their user record, which Clerk fills)
when they may set the contact, so the Billing page can offer it; it is null for a member, for the
operator, and once a contact is saved. Nothing is stored until someone saves it.

**Setting it.** `set_billing_contact` is for the account's owners and admins (who hold `admin` on the
customer) and the operator; a member is refused `unauthorized`. It replaces the whole contact: a
field left out is emptied. Whitespace around each field is trimmed, and a field the checks refuse
is `invalid_input` with its path (`/email`, `/country`). The central audit records kind
`billing.contact.set` on `customer/<id>/billing` with `fields`, the names of the fields that were
set, never their values, `provider` and `provider_update`.

**What reaches Stripe.**

- At the account's first invoice, `POST /v1/customers` carries `email`, `name` (the contact's, else
  the account's) and the address fields the contact has: `address[line1]`, `address[line2]`,
  `address[city]`, `address[postal_code]`, `address[state]`, `address[country]`. Its key stays
  `lake-customer-<customer>`, so a creation that failed after Stripe answered and is retried within
  24 hours with a changed contact is refused by Stripe's idempotency check until the key expires.
- Each later save, while the account has a Stripe customer, is `POST /v1/customers/<cus_...>` with
  `email`, `name` and all six address fields, so one the contact no longer has is cleared (`address=`
  when it has no address at all), under the key `lake-customer-<customer>-contact-<updated_at>`: one
  key per save.
- `set_billing_contact` answers `provider_update`: `updated`; `none` when the account has no
  customer yet (it gets the contact at creation); or `failed` with Stripe's message in
  `provider_error`. A refusal never undoes the save: the contact stays stored and is sent again at
  the next save.

The manual provider has no customer, so its accounts always answer `none`. Stripe emails each
invoice to the contact's email ([below](#emailing-invoices)).

## Emailing invoices

[lake#857](https://github.com/oramasearch/lake/issues/857): Stripe emails each invoice to the
account's billing email, so an account that does not open lake that month still learns it owes
money. Invoices are still finalized with `auto_advance=false`, so Stripe sends nothing by itself;
lake asks it once.

- **When.** `issue_invoice` asks the provider to send an invoice (`send_payment_request`) only after
  lake has stored it, and only when the account has a [billing contact](#billing-contact) and the
  invoice was issued through a provider that has a request to send (Stripe; the manual provider has
  none). A notice that arrived before lake's invoice existed would be recorded unmatched and its
  redelivery treated as a duplicate, so the payment would never be applied; sending after the store
  rules that out. Without a billing contact nothing is sent, and the invoice is paid from lake's
  invoice page as before.
- **Stripe.** `POST /v1/invoices/<id>/send` under the key
  `lake-invoice-<customer>-<month>-<n>-send`. Stripe emails the hosted invoice link to the
  customer's email, which is the billing contact's. The status in its answer becomes
  `provider_status` (`open`).
- **A failed send** does not fail the issue: the invoice is stored, due, and payable on lake's
  invoice page. Its `provider_status` becomes `send_failed`, and the central audit records
  `billing.invoice.send_failed` (action `issue_invoice`, outcome `failure`, with the invoice and
  Stripe's message) beside `billing.invoice.issue`, whose `send` detail is `sent`, `failed` or
  `none`. The status moves only from the one lake read (`set_invoice_provider_status`), so a
  payment notice applied meanwhile is kept.
- **Resending.** `resend_invoice` (operator, instance administrator, as `issue_invoice`) sends a due
  invoice again, after a failed send or as a reminder; a send that goes through moves `send_failed`
  back to Stripe's status and leaves any other status, such as `payment_failed`, as it was.
  Each resend has its own key, `lake-invoice-<customer>-<month>-<n>-send-<uuid>`, because Stripe
  answers a key's first answer again for 24 hours, a refusal included, so a resend under the
  first key would only repeat the failure. A refusal answers `store_unavailable` with Stripe's
  message and leaves the invoice `send_failed`. An invoice that is not due, one issued without a
  provider request, and an account without a billing contact are `conflict`; an invoice issued
  through a provider that is not configured now is `store_unavailable`. Audited
  `billing.invoice.send` or `billing.invoice.send_failed`.
- **Receipts.** Whether Stripe also emails a receipt when an invoice is paid depends on the Stripe
  account's customer-email settings, which Lake and Cloud v1 share.
  [orama-bridge#200](https://github.com/oramasearch/orama-bridge/issues/200) records them; nothing
  here changes them.
- **Test mode.** Stripe sends no email for a `send` in test mode or a sandbox, though it still emits
  `invoice.sent` ([Stripe: send an invoice](https://docs.stripe.com/api/invoices/send)). The `dev`,
  `testing` and `staging` sandboxes therefore prove that lake made the request, not that mail
  arrived. Delivery is first seen on the first real live invoice
  ([orama-operator#643](https://github.com/oramasearch/orama-operator/issues/643), step 3).

## Webhooks

`POST /billing/stripe/webhook` receives Stripe's events. It has no lake credential: the
`Stripe-Signature` header is the credential. **A notice that does not authenticate is not audited one by one** (LSR-196, lake#1002): it is
answered `400` and counted, and at most one `billing.payment.notice` `denied` event a minute records
the reason and `refused_since_last_summary`, so unauthenticated traffic cannot append to the central
audit log at will.

1. **Signature.** `Stripe-Signature: t=<unix seconds>,v1=<hex>[,v1=...]`; lake computes
   HMAC-SHA256 of `<t>.<raw body>` with the webhook secret (OpenSSL) and compares it with every
   `v1` in constant time. No match is `400 invalid_signature`. When the check fails, lake reads the
   keys again once (a rotated webhook secret) before refusing.
2. **Replays.** A `t` more than 300 seconds from lake's clock, either way, is `400
   stale_timestamp`: a captured request cannot be replayed later. Within the window, the event id
   decides.
3. **Whose event it is** (lake#855). Stripe cannot filter an endpoint's events by metadata, so in a
   shared account each endpoint receives Cloud v1's and the other Lakes' events of the types it
   subscribes to. After the signature check, and before anything is recorded, lake reads the
   event's object (`parse_stripe_event`):

   | Object | This Lake's when |
   |---|---|
   | An invoice (`invoice.*`) | its `metadata.lake_customer_id` is set (an invoice lake issued), or, for an invoice Stripe generated from a subscription, which does not copy the subscription's metadata, its `subscription_details.metadata.lake_order_id` is |
   | A Checkout Session (`checkout.session.*`) | its `metadata.lake_order_id` is set |

   and, in both, `lake_instance` is this Lake's public host or absent (an object tagged before
   lake#855). Any other is foreign, Cloud v1's or another Lake's: the route answers `200
   {"received": true, "ignored": true}` and writes no `billing_payment_events` row and no audit
   event. An unsigned or stale foreign event is still refused (steps 1 and 2).
4. **Once per event.** Each applied event is recorded in `billing_payment_events` (provider, event
   id, type, Stripe invoice, lake invoice), in the same transaction that moves the invoice. The same
   event again (Stripe's own retry, or a replay inside the window) answers `200 {"duplicate":
   true}` and changes nothing.
5. **What moves.** Only a due invoice moves:

   | Event | Lake invoice | `provider_status` |
   |---|---|---|
   | `invoice.paid` | `due` → `paid` (`paid_at`) | `paid` |
   | `invoice.paid` with `paid_out_of_band: true` | unchanged (`due`): marked paid in Stripe without a payment, which anything else with a key for the account can do (lake#1033); it never activates an order either | `paid_out_of_band`, for an operator to review |
   | `invoice.payment_failed` | stays `due` | `payment_failed` |
   | `invoice.voided` | `due` → `void` (`voided_at`, note "Voided in Stripe") | `void` |

   A paid invoice is never voided or failed by a later event; a void lake asked for is confirmed
   (`provider_status` becomes `void`). Other event types answer `200 {"ignored": true}`; an invoice
   tagged as this Lake's that lake did not issue answers `200 {"matched": false}`.
6. **Audit.** Every event that moves an invoice is a central audit event (decision 52) on
   `customer/<id>/billing`, action `payment_notice`, kind `billing.invoice.paid`,
   `billing.invoice.void` or `billing.invoice.payment_failed`, principal `payment-provider`, with
   the event id and both invoice ids. Refused notices are recorded `denied` on `billing/stripe`,
   unmatched ones `ignored`.

The route answers `404` where billing is off or the manual provider is in use, `413` for a body
over 1 MiB, and `503` (Stripe retries) when the keys or the store cannot be read.

**Order sheet Checkout events** (lake#831): the same route takes `checkout.session.completed`,
`checkout.session.async_payment_succeeded`, `checkout.session.async_payment_failed` and
`checkout.session.expired` for the apex order sheet's paid orders. They move a `cloud_orders` row
(`pending_payment` → `paid` or `cancelled`), never an invoice, once per event id through the same
`billing_payment_events` row; the order's event row carries them to the central audit log. A
session that is not this Lake's (step 3) is ignored the same way.
Contract: [orders.md](orders.md#paying).

**Endpoint to register in the Stripe dashboard:** `https://<lake host>/billing/stripe/webhook`
(on dev, `https://lake.oramadev.net/billing/stripe/webhook`), with the events `invoice.paid`,
`invoice.payment_failed` and `invoice.voided`, on the Lake that serves the apex order sheet the
four `checkout.session.*` events above, and for the [balance](#stripe-1) `checkout.session.completed`,
`checkout.session.async_payment_succeeded`, `setup_intent.succeeded`, `payment_intent.succeeded` and
`payment_intent.payment_failed`. Its signing secret (`whsec_...`) is the `webhook_secret`
below. A restricted key (`rk_`) must also be allowed to write Checkout Sessions.

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

## Balance

[lake#1114](https://github.com/oramasearch/lake/issues/1114), [liquid-bridge decision
67](https://github.com/oramasearch/liquid-bridge/blob/main/docs/decisions.md), Steve, 2026-10-24:
*"let's add an account balance... they can add $20 to start... it will nag them and stop them from
creating things when it's too far over their balance (default $50 over)"*, and *"it should send them
emails when they're about to hit their limits or face interruptions"*, then *"the email should link
to a place where they can top up ... and auto top up (default off)"*.

**lake.live is prepaid.** Each account (a person's or an organization's) has a balance in US
dollars, kept in millionths:

- **Credits** are paid top-ups: a Stripe Checkout the account's owner starts (`start_top_up`), or an
  [auto top-up](#auto-top-up) off a saved card. Orama can also give an account a credit
  ([credits from Orama](#credits-from-orama)): a credit of kind `grant`, spent before paid money.
- **Charges** are the account's usage at the price list in force, one per month, refreshed every few
  minutes ([below](#charges)).
- **The balance is credits minus charges.**

It exists only where billing is on (`LAKE_BILLING`); an enterprise install has none of it, and no
call is gated or warned.

**Where it lives** (decision 56). The balance is billing, so it is in lake's control PostgreSQL
(migration `0099_account_balances`, guard `account_balances_guard`): `account_balances` (each
account's row: its settings, the last refresh, which crossings' emails went, auto top-up),
`account_balance_credits` (one row per payment, unique on the provider and its Checkout Session or
PaymentIntent, and one per credit from Orama, migration `0100_balance_grants`) and `account_balance_charges` (one row per account and month, with the price list
and, once invoiced, the invoice). Usage stays in each workspace Zetta: only the priced amount of a
month reaches PostgreSQL, as an invoice's lines do.

### States

| State | When | What happens |
|---|---|---|
| `ok` | the balance is at or above the warning amount (`LAKE_BALANCE_WARNING_USD`, $10) | nothing |
| `low` | from $0 to under the warning amount | the banner and the API warning |
| `negative` | below $0, above the overdraft limit | the banner, the warning, and the emails below |
| `blocked` | at or past the overdraft limit (`LAKE_BALANCE_OVERDRAFT_USD`, $50 below zero), or the account has never topped up and has no credit from Orama left (its limit is $0) | paid work answers `402 payment_required` |

**New accounts start at $0 and add their first top-up before any paid action** (`blocked`, reason
`first_top_up`). The overdraft is credit Orama extends only to accounts that have paid once. This is
the default chosen for lake#1114: Steve said "add $20 to start", and it denies free usage to unpaid
sign-ups. **The first top-up is exactly `LAKE_BALANCE_FIRST_TOP_UP_USD` ($20)**; later ones are any
amount from `LAKE_BALANCE_MINIMUM_TOP_UP_USD` ($20), in whole cents, at most $10,000 at a time.

`add_micros` is how much to add: the first top-up, else what brings the balance back over the
warning amount, rounded up to a whole dollar and at least the minimum. Every link to add funds is
one console page, **`/billing/add-funds?customer_id=<account>`**, under the Lake's origin
(`LAKE_EXTERNAL_ORIGIN`, else `https://<LAKE_PUBLIC_HOST>`). It signs a visitor in first.

**Exempt** from the gate, the warning and the emails: the operator credential and the instance
administrator (callers), and an account whose organization has a paid cloud order (`paid`,
`provisioning` or `ready`, with its subscription current: `cloud_order_desired_state`) whose
credit covers it (`exempt: "paid_order"`; such an account reads `ok`).

### Charges

The store root's owner refreshes every account with a balance row every
`LAKE_BALANCE_REFRESH_SECONDS` (300). For each month from the first top-up's (or the first credit
from Orama's, whichever came first) to this one it works
out the month's bill as `bill_preview` does, from `usage_over_time` at the list in force for the
month, and keeps its total as the month's charge. It reads usage without a capability call of its
own (the handler directly), so a refresh every few minutes does not put an audit event per account
in the central log.

- **A month's charge is its bill.** The same lines and rounding as the invoice: a month's charge
  and the month's invoice agree to the micro.
- **A failed read holds the balance.** If any workspace's usage cannot be read, nothing is written:
  every month keeps its last charge, `refresh_error` says which workspace, `refresh_failed_at` when.
  Lake never guesses.
- A month whose charge was read after it ended (an hour's margin) is final and not read again. A
  month without a price list is charged $0.
- Usage between refreshes can pass the limit by a few minutes' worth (decision 67's cost).

### Invoices and the balance

**An issued invoice records what the balance paid.** For an account that has topped up,
`issue_invoice` freezes the month's lines as before, then issues the invoice **paid** with provider
`balance` (`provider_status` `paid`, instructions "Paid from your Lake account balance"), asks no
provider for payment and sends no email. The month's charge is frozen to the invoice's total
(`account_balance_charges.invoice_id`), and a refresh never moves it again. A balance below $0 is
money the account owes, collected by its next top-up, never by a second bill. An account on a credit
from Orama alone is invoiced the same way: the credit paid. An account with neither a top-up nor a
credit (an existing account from before the balance, or one a paid order covers) is invoiced as
before, through Stripe.

### The gate

Every capability that creates, starts or grows something that costs money is marked
`Capability::starts_paid_work`. **One list** marks them, `paid_work_capabilities()` in
`src/api/balance.cpp`, applied by `Registry::add`, so the engine's capabilities registered from its
schema are marked too:

- the quickstart process: `start_quickstart`, `train_quickstart`, refused before any step starts
  ([quickstart.md](quickstart.md#the-balance));
- apps: `create_app`, `deploy_app`, `start_app`, `restart_app`, `scale_app`, `rollback_app`,
  `build_app`;
- services and stores: `create_service`, `create_service_and_bind_to_app`, `connect_service`,
  `create_store`, `set_workspace_storage`; workspaces: `create_workspace`, `start_workspace`,
  `update_workspace`;
- models and indexes: `train_model`, `retrain_model`, `register_model`, `load_drive_model`,
  `expose_model`, `create_index`, `expose_index`;
- runs, training and experiments: `start_run`, `run_pipeline`, `deploy_pipeline`, `run_solution`,
  `start_solution`, `run_methodology`, `validate_methodology`, `start_experiment`;
- agents: `launch_agent`, `schedule_agent`, `agent_run`, `agent_turn`, `chat_with_agent`,
  `send_ask_message`, `create_mcp_connection_agent`;
- image builds: `build_image` (and `build_app`); the model gateway: `model_completion`; speech:
  `transcribe`, `create_transcript`;
- storage growth: `upload_dataset`, `generate_dataset`, `create_dataset_from_query`,
  `load_drive_dataset`, `begin_drive_upload`, `upload_drive_files`, `upload_drive_entries`,
  `import_repository`, `import_github_repository`, `push`.

`Registry::call` checks the mark after authorization and before the handler, in one place: no
handler checks the balance. While the account the call charges is `blocked`, it answers **`402
payment_required`** with `error.details` `{customer_id, balance_micros, overdraft_micros,
add_micros, add_funds_url, reason}` and a message that says the balance, the limit, how much to add
and where. The refusal is audited like any refused call. The account a call charges is the
workspace it acts on, else its `customer_id`, `repository_id` or `organization_id`, else the
caller's personal account.

**Reading, signing in, stopping and deleting stay open**, so an account can always lower its spend
(`update_workspace` is refused only with `desired: running`; stopping or renaming a workspace always
passes, [#1122](https://github.com/oramasearch/lake/issues/1122));
**running things keep running** (stopping them for an unpaid balance is a later step, sleep never
delete, orama-operator#897). `PaidWorkContract.EveryCapabilityThatStartsPaidWorkIsMarked` fails when
a capability whose name says it creates, starts, runs, deploys, trains, launches, builds, imports
or uploads something is neither on the list nor classified as free in the test, with why.

### The warning

While the account a call acted on is `low`, `negative` or `blocked`, the answer carries
`warnings: [{code, message, customer_id, balance_micros, add_micros, add_funds_url}]`, `code`
`balance_low`, `balance_negative` or `balance_blocked`, beside its value or error. It is added in
the one shared answer path, `Registry::call` for the outermost call, so every capability carries it
on REST and MCP alike and none adds it itself ([errors.md](errors.md)). The operator is never
warned. `viewer.balance` gives the web app the current account's balance for the banner.

### Emails

Through the Resend client (`LAKE_RESEND_API_KEY`, from `LAKE_RESEND_FROM`, by default
`Lake <orders@lake.live>`), by the refresh, to the account's [billing contact](#billing-contact),
else its owner's email (the person's own, or an organization owner's):

| Crossing | When | Subject |
|---|---|---|
| near the limit | the balance is within the warning amount ($10) of the overdraft limit | Your Lake balance is close to its limit |
| below $0 | the balance passes $0 | Your Lake balance is below $0 |
| blocked | the balance reaches the limit: paid work is refused | Lake has stopped starting paid work on your account |
| auto top-up off | a declined or failed auto top-up turned it off | Your Lake auto top-up was turned off |

- **Once per crossing.** The crossing is marked on the account (`*_notified_at`) before the send,
  so an overlapping refresh never sends it twice, and unmarked when the send fails, so the next
  refresh tries again. The most serious crossing not yet told is sent, and covers the milder ones.
  The Resend idempotency key is `lake-balance-<account>-<crossing>-<credits so far>`.
- **Reset when funds are added.** Every credit clears the three balance marks.
- Each says the balance, how much to add and the Add funds link. **It carries nothing a person
  typed**: no account, person or workspace name. An account that has never topped up and never had
  a credit from Orama gets none; one on a credit alone is told near its $0 limit and at it.
- A credit from Orama sends its own email, once (below).
- Each sent email is audited centrally, `billing.balance.email`, with the crossing.

### Auto top-up

**Off by default.** The account's owners and admins turn it on in Billing → Auto Top-Up
(`set_auto_top_up`) with a threshold (default: when the balance falls below $5) and an amount
(default $20, never less than the minimum top-up):

1. **Saving a card.** Turning it on without a saved card answers a Stripe Checkout in `setup` mode
   on the account's Stripe customer (`checkout_url`); the row waits (`awaiting_card`). Stripe's
   `setup_intent.succeeded`, tagged with the account, saves the payment method's id (`pm_...`) and
   turns auto top-up on. Lake never sees the card. `replace_card: true` saves another.
2. **Charging.** When a refresh finds the balance below the threshold, Lake claims the crossing on
   the row (one charge in flight, at most `LAKE_AUTO_TOP_UPS_PER_DAY`, 5, a UTC day) and charges the
   card off-session: `POST /v1/payment_intents` with `customer`, `payment_method`, `off_session=true`,
   `confirm=true`, under the idempotency key `lake-auto-top-up-<account>-<n>`, one per account and
   crossing, so a refresh never charges twice.
3. **The credit comes from the webhook**, exactly like a top-up: `payment_intent.succeeded` credits
   it once and clears the charge in flight. One that never answers is given up after a day.
4. **A decline turns it off.** A refused charge, one that needs the person to authenticate
   (`requires_action`), or `payment_intent.payment_failed` turns auto top-up off with the reason
   (`off_reason`), emails the owner once with the Add funds link, and leaves the nag and the limit
   in force.

Starting, saving, crediting and failing are audited centrally (`billing.balance.auto_top_up_set`,
`billing.balance.auto_top_up_on`, `billing.balance.auto_top_up_started`,
`billing.balance.auto_top_up_failed`, `billing.balance.credit`).

### Stripe

- **Top-up.** `POST /v1/checkout/sessions`, `mode=payment`, on the account's Stripe customer (made
  as for an invoice), one line of the `lake_usage` [product](#stripe) at the amount inline (a top-up
  prepays usage, so no new product is needed), `client_reference_id` and
  `metadata[lake_customer_id]` the account, `metadata[lake_balance]=top_up`, and the tags. It
  returns to `/billing/add-funds?customer_id=…&top_up=done` or `&top_up=cancelled`. The key is
  `lake-top-up-<account>-<uuid>`, a new one per attempt.
- **Card setup.** `mode=setup`, `payment_method_types[0]=card`, the same tags on the session and on
  `setup_intent_data[metadata]` with `lake_balance=card_setup`; it returns to
  `/billing?customer_id=…&auto_top_up=saved` or `cancelled`.
- **Events.** `checkout.session.completed` with `payment_status=paid`, and
  `checkout.session.async_payment_succeeded`, credit a top-up once per event id
  (`billing_payment_events`) and once per session (`account_balance_credits`); a session not paid
  yet is acknowledged and waits. `setup_intent.succeeded`, `payment_intent.succeeded` and
  `payment_intent.payment_failed` carry auto top-up. An event whose object has no
  `lake_customer_id` and `lake_balance`, or whose `lake_instance` is another Lake's, is foreign:
  acknowledged with nothing recorded, as for invoices (lake#855).
- **The events to register** for lake.live's endpoint, added by the operator's
  `lakeStripeWebhookEvents`: `setup_intent.succeeded`, `payment_intent.succeeded`,
  `payment_intent.payment_failed`, beside the `checkout.session.completed` and
  `checkout.session.async_payment_succeeded` the order sheet registers.

### Credits from Orama

[lake#1134](https://github.com/oramasearch/lake/issues/1134), Steve, 2026-10-24: *"I need the
ability to give cloud credits to certain users"*. The defaults below are lake#1134's; Steve may
change any of them.

**What a grant is.** The operator or the instance administrator gives one account a credit
(`grant_balance_credit`): a design partner, a prize, a support make-good. It is a row of
`account_balance_credits` of kind `grant`, provider `orama`, beside the paid top-ups, so **the
balance is still the credits minus the charges**. It records the amount (whole cents, $0.01 to
$10,000), a required `reason` (shown to the account), who gave it, an optional `expires_at`, and
the account's charges at that moment.

- **Not cash.** It is never refunded or paid out, and is listed apart from paid money ("Credit from
  Orama", with its reason, expiry and what is left).
- **No overdraft.** A grant never sets `first_top_up_at`. An account that has never paid spends a
  grant down to $0 and stops there (`blocked`, reason `first_top_up`, limit $0); its first paid
  top-up unlocks the overdraft as always.
- **It clears the low-balance marks**, like any credit, so the next crossing is told again.
- **One email**, through the same Resend path, keyed `lake-balance-grant-<credit id>`: the amount,
  the balance after, the expiry, that it is spent before paid money and is not cash, and a link to
  the Billing page. The reason is shown on the page, never mailed (emails carry nothing a person
  typed). The answer says whether it went (`emailed`, `email_error`).
- **A retry is safe.** `reference` is an idempotency key: the same reference grants once, emails
  once, and answers `duplicate: true`.
- **By email.** `email` names a person's personal account (case-insensitive); an organization's
  account is named by its `customer_id`.

**How charges fall on grants (the rule).** Usage is paid by grants before paid money. A grant pays
only for usage charged **after it was given** (the charges then are recorded on it) and, once it has
ended, before it ended. When several grants could pay, **the one that expires first pays first**,
then the oldest. Lake works this out from the recorded charges (`grant_allocation` in
`include/lake/control/store.hpp`), in integer micros.

**Expiry and revocation.** When a grant ends, **its unused part stops counting**
(`forfeited_micros`) and **what it paid for stays paid**; paid money is untouched. For example, with
$20 paid and a $25 grant: $10 of usage is paid by the grant; when the grant expires, its $15 unused
stops counting and the balance is $20, the paid money, whole.

- **Expiry** is applied by the balance refresh, after a read of every workspace's usage succeeds, so
  the unused part is measured against the usage up to then. An expired grant counts until that
  refresh (at most `LAKE_BALANCE_REFRESH_SECONDS` late, longer while usage cannot be read: never
  against charges that were not read). Audited `billing.balance.credit_expired` (principal `lake`).
- **`revoke_balance_credit`** reads the account's usage first, then ends the grant at once. It never
  takes back money already spent; it is refused (`store_unavailable`) when usage cannot be read, and
  `conflict` when the grant already ended. Audited `billing.balance.credit_revoked`.
- A grant ends once: both run in one transaction that locks the account's grants.

**Giving a credit on lake.live.** In the console, the operator signs in and opens **Customers →
Give credit** (an account ID or a person's email), or an account's **Billing → Give credit**; a
credit that still counts has **Revoke** in the Billing page's history. Over REST or MCP, call
`grant_balance_credit` with that Lake's admin token, the operator credential: it is the Secret
**`lake-<name>-admin`**, key **`token`**, in the Lake's namespace on its cluster, where `<name>` is
the `Lake` resource's name (the orama-operator creates it and gives it to lake as
`LAKE_ADMIN_TOKEN`). Read it with `kubectl get secret` on that cluster when you need it; never paste
the value into a document, an issue or a chat:

```bash
curl -s https://lake.live/v1/grant_balance_credit \
  -H "Authorization: Bearer $LAKE_ADMIN_TOKEN" -H 'content-type: application/json' \
  -d '{"email":"pat@example.com","amount_micros":25000000,"reason":"Design partner",
       "expires_at":"2027-01-31T23:59:59Z","reference":"partner-pat-2026-10"}'
```

### Coupon codes

[lake#1156](https://github.com/oramasearch/lake/issues/1156), Steve, 2026-10-24: *"create coupon
codes that will grant someone a number of credits that I can use in my ads"*. The defaults below
are lake#1156's; Steve may change any of them.

**What a code is.** The operator or the instance administrator creates a code (`create_coupon`),
such as `LAKE50`, worth an amount of credit. Whoever redeems it (`redeem_coupon`) gets an ordinary
[credit from Orama](#credits-from-orama): the redemption calls the same grant path as
`grant_balance_credit` (`give_balance_grant` in `src/api/balance.cpp`), so the spending order,
expiry, no-overdraft rule, email and idempotency are the grant's, never a second mechanism.

- **The grant.** Kind `grant`, provider `orama`, reason `Coupon code <CODE>` (shown to the account),
  `granted_by` `coupon:<coupon id>`, `expires_at` the redemption plus the code's `credit_days`
  (default 90), and the reference `coupon:<coupon id>:<customer id>`, so a retry, or a redemption
  interrupted between its claim and its grant, grants once. The grant's email goes once
  (`lake-balance-grant-<credit id>`).
- **The code.** Typed (4 to 32 letters and digits; blanks and hyphens are dropped; case does not
  matter) or, when left out, generated: 16 characters from `ABCDEFGHJKMNPQRSTVWXYZ23456789` (no
  look-alikes), about 78 bits from OpenSSL's random source, so it cannot be guessed. Codes are
  stored normalized (upper case), and `LAKE50`, `lake-50` and `Lake 50` are one code.
- **Its settings.** The amount (whole cents, $0.01 to $10,000), `credit_days` (1 to 3,650), an
  optional `expires_at` (when the code stops working, UTC, within 10 years), an optional
  `max_redemptions` (the total cap), `new_accounts_only` (default **true**, the default for ads) and
  an optional `campaign` label (lower-case letters, digits, `.`, `_`, `-`; up to 64) for
  attribution.
- **Stopping it.** `disable_coupon` (by `coupon_id` or `code`) refuses later redemptions at once;
  credit already given stays. Calling it again changes nothing.

**Who may redeem, and how often.**

- **A person.** `redeem_coupon` takes `customer_id` and `code`; the caller needs `admin` on the
  account (owners and admins), and must be a person signed in: the operator credential and an
  agent's credential are refused (`unauthorized`).
- **One redemption per account and per verified email.** The person's email (the one on their
  account: Clerk's verified primary address on lake.live) is kept only as a SHA-256 digest of its
  normalized form: lower-cased, a `+tag` dropped, and for Gmail (`gmail.com`, `googlemail.com`) the
  dots of the local part dropped. So one inbox cannot redeem a code twice through two accounts. A
  person without an email cannot redeem (`conflict`, reason `no_verified_email`).
- **New accounts only** refuses an account created before the code (`not_new_account`) and one that
  already has any credit (a top-up or a grant), any charged usage, or a non-void invoice with a
  total (`account_used`). A new account cannot start paid work before its first credit, so it has no
  charges to hide.
- **The cap, the expiry and disabling** are checked under the coupon's row lock in the same
  transaction that records the redemption (`used_up`, `expired`, `disabled`).
- **Failed attempts are limited**: 10 refusals an hour per person and per client address (the
  ingress-derived address that keys sign-in budgets), counted in `billing_coupon_attempts` by
  SHA-256 digests of the keys. Past it, every attempt answers `rate_limited` (429), the right code
  included, until the hour has passed. A refusal for a server error is not counted.

A refusal answers `not_found` (an unknown code) or `conflict`, with `error.details.reason` one of
`unknown`, `disabled`, `expired`, `used_up`, `already_redeemed`, `email_redeemed`,
`not_new_account`, `account_used`, `no_verified_email`, and a sentence a person can read
(`"This code has expired."`).

**Audit and privacy.** Each redemption is audited centrally on `customer/<id>/billing`:
`billing.coupon.redeemed` with who (the principal), the code, the campaign, the coupon, redemption
and credit ids, the amount and the expiry. A refusal is `billing.coupon.refused` with its reason
and a 12-hex-digit SHA-256 prefix of what was typed (`code_digest`), never the typed code: a guess
is not written down anywhere, and request logs carry route patterns only. Creating, listing and
disabling are audited on `billing/coupons` (`billing.coupon.created`, `billing.coupon.listed`,
`billing.coupon.disabled`).

**The ads report.** `list_coupons` answers every code, newest first (`campaign`: one campaign's
only), with its `state` (active, disabled, expired, used_up), `redemptions` (claimed, what the cap
counts) and, per code: `redeemed` (redemptions that became a credit), `topped_up` (redeeming
accounts whose first paid top-up came after their redemption), `spend_micros` (what those accounts
were charged for usage after their credit was given) and `paid_micros` (what they paid in top-ups
since redeeming). `campaigns` sums the same per campaign. Counts and sums only: no workspace data,
no names. An account that redeemed two codes of one campaign is counted under each.

**Carried through sign-up.** An ad links to `https://lake.live/?code=LAKE50`. The apex region
picker (and the order sheet, when it is on) posts back to the same path and query and continues to
the chosen region's host with it (`region_continue_url`), and says "Your code LAKE50 goes with you".
On the region's host the web app keeps the code in the browser for a day and redeems it on the first
signed-in page for the person's own account ([Coupon codes in the console](../ui/balance.md#coupon-codes)).

**Regions.** Each lake.live region is a separate Lake with its own control plane, and Lake has no
way to call a sibling region today, so **a code works only in the region it was created in**. For an
ad that reaches several regions, create the same code (the same string, amount and settings) in each
region: in each region's console (**Administration → Coupons**), or with `create_coupon` and that
region's admin token. `create_coupon` answers `region` and `other_regions` to remind you. Each
region counts its own redemptions and cap. Creating it in every region from one form is a follow-up
([lake#1159](https://github.com/oramasearch/lake/issues/1159)).

**Where it lives.** Billing, so in the control plane (decision 56): `billing_coupons`,
`billing_coupon_redemptions` and `billing_coupon_attempts`, migration `0101_coupons`
([schema](../control-plane/schema.md#coupon-codes-migration-0101_coupons)).

**On lake.live** (an ad for $50 lasting 90 days):

```bash
curl -s https://eu.lake.live/v1/create_coupon \
  -H "Authorization: Bearer $LAKE_ADMIN_TOKEN" -H 'content-type: application/json' \
  -d '{"code":"LAKE50","amount_micros":50000000,"credit_days":90,
       "expires_at":"2026-12-31T23:59:59Z","campaign":"google-2026-11"}'
# then the same on https://us.lake.live with that region's admin token
curl -s "https://eu.lake.live/v1/list_coupons?campaign=google-2026-11" \
  -H "Authorization: Bearer $LAKE_ADMIN_TOKEN"
```

`$LAKE_ADMIN_TOKEN` is the region's Secret `lake-<name>-admin`, key `token` (see
[Giving a credit on lake.live](#credits-from-orama)); never paste it anywhere.

### Balance capabilities

| Capability | Kind | Who | Input | What it does |
|---|---|---|---|---|
| `account_balance` | read | `read` on the customer | `customer_id` | `{customer_id, currency, balance_micros, credits_micros, charges_micros, overdraft_micros, limit_micros, state, reason, add_micros, add_funds_url, first_top_up_micros, minimum_top_up_micros, first_top_up_at, exempt, refreshed_at, refresh_error, auto_top_up, credits, charges}`; `credits` (the last 50) and `charges` (by month) newest first. |
| `start_top_up` | action | `admin` on the customer (owners, admins), operator | `customer_id`, `amount_micros?` | A Stripe Checkout that adds the amount: `{customer_id, amount_micros, checkout_url, session_id}`. The first top-up is exactly the first amount; later ones at least the minimum, whole cents. `conflict` where card payment is not set up. |
| `set_auto_top_up` | action | `admin` on the customer, operator | `customer_id`, `enabled`, `threshold_micros?`, `amount_micros?`, `replace_card?` | Turns auto top-up on or off and sets its threshold and amount; `checkout_url` when a card must be saved first. |
| `set_balance_overdraft` | action | operator, instance administrator | `customer_id`, `overdraft_micros?` | One account's overdraft in place of the instance's; left out, the instance's again. |
| `refresh_account_balance` | action | operator, instance administrator | `customer_id` | Refreshes one account now as the owner loop does, and answers `{balance, refresh}`; `refresh.grants_expired` counts the credits from Orama it ended. |
| `grant_balance_credit` | action | operator, instance administrator | `customer_id` or `email`, `amount_micros` (10,000 to 10,000,000,000, whole cents), `reason` (1 to 1,000 bytes), `expires_at?` (UTC `YYYY-MM-DDTHH:MM:SSZ`, future, within 10 years), `reference?` (1 to 200 of `A-Z a-z 0-9 - _ . :`) | Gives the account a [credit from Orama](#credits-from-orama): `{customer_id, duplicate, credit, emailed, email_error?, balance}`. |
| `revoke_balance_credit` | action | operator, instance administrator | `credit_id`, `reason?` | Ends a grant after reading usage: `{customer_id, credit, revoked_micros, balance}`; only the unused part stops counting. `invalid_input` for a paid top-up, `conflict` once it ended. |
| `create_coupon` | action | operator, instance administrator | `amount_micros` (10,000 to 10,000,000,000, whole cents), `code?` (4 to 32 letters and digits after normalizing; left out: generated), `credit_days?` (1 to 3,650, default 90), `expires_at?` (UTC, future, within 10 years), `max_redemptions?` (1 to 10,000,000), `new_accounts_only?` (default true), `campaign?` | Creates a [coupon code](#coupon-codes): `{coupon, link, region, other_regions}`; `link` is `https://<apex>/?code=<CODE>` (this Lake's origin without an apex). `conflict` when the code is taken. |
| `list_coupons` | read | operator, instance administrator | `campaign?` | `{coupons, campaigns}`: each code with `state`, `redemptions`, `redeemed`, `topped_up`, `spend_micros`, `paid_micros`; the same summed per campaign. |
| `disable_coupon` | action | operator, instance administrator | `coupon_id` or `code` | Stops the code at once: `{coupon}`. Idempotent. |
| `redeem_coupon` | action | `admin` on the customer, a person (not the operator or an agent) | `customer_id`, `code` | Gives the account the code's credit from Orama: `{customer_id, code, amount_micros, expires_at, message, credit, emailed, email_error?, balance}`. Refusals: `not_found`/`conflict` with `details.reason`, `rate_limited` past 10 failures an hour per person or client. |

All are audited centrally on `customer/<id>/billing` (`billing.balance.read`,
`billing.balance.top_up_started`, `billing.balance.auto_top_up_set`,
`billing.balance.overdraft_set`, `billing.balance.refresh`, `billing.balance.credit_granted` with
who, how much, why and the expiry, `billing.balance.credit_revoked`, `billing.coupon.redeemed`,
`billing.coupon.refused`); the coupon administration on `billing/coupons`.

`account_balance` also answers `first_grant_at` and `grants_unused_micros`, and each credit's
`label`; a grant's row adds `reason`, `expires_at`, `ended_at`, `end_reason` (`expired` or
`revoked`), `forfeited_micros`, `counting_micros` and, while it counts, `unused_micros`.

### Verify the balance

- `Balance.*` (`tests/test_balance.cpp`): the view and its math (states, the first top-up, the
  overdraft and the operator's, `add_micros`, the settings from the environment); a top-up credited
  once per payment from signed Stripe events (a replay and the async event credit nothing more,
  another Lake's is ignored); the first top-up exactly $20 and later ones at least $20 in whole
  cents, the manual provider refusing; a month's charge equal to `bill_preview`, a failed usage read
  holding the balance with the reason; an issued invoice paid by the balance and freezing its
  month; the gate (402 before the first top-up and past the limit with its details, reads, stops
  and deletes open, the operator exempt, the operator's overdraft, billing off ungated, a paid order
  exempt); the warning on answers while low, negative and blocked, once, and `viewer.balance`; the
  emails once per crossing, to the owner then the billing contact, reset by a credit, retried after
  a failed send, with no name in them (a fake Resend sender); auto top-up off by default, one charge
  per crossing with its key, credited only from the webhook, the daily limit, a decline turning it
  off with one email, and a webhook failure doing the same.
- `StripeBalance.CheckoutsChargesAndEventsAreLakesOwn` (`tests/test_balance.cpp`, a fake Stripe):
  the top-up and card-setup sessions' fields and keys, the off-session PaymentIntent, a decline and
  a `requires_action` answer as failures, and each event's notice, foreign ones included.
- `PaidWorkContract.EveryCapabilityThatStartsPaidWorkIsMarked`: the list, the marks and the names.
- `MemoryBilling.AccountBalancesFollowTheContract` and `PostgresBilling.AccountBalancesFollowTheContract`
  (`tests/test_postgres.cpp`): the migration's tables and guard, the compare-and-swap row, credits
  once per event and per payment, charges frozen by an invoice, and the tables' checks.
- `BalanceGrant.*` (`tests/test_balance.cpp`): only the operator or the instance administrator gives
  or revokes (the account's own owner and a stranger are refused, and audited as denied); the
  amount, reason, expiry, reference and account checks; a grant by id and by email credited,
  listed as "Credit from Orama", audited and emailed once (a retry with the same reference neither
  grants nor emails again); blocked to ok on a grant, the marks cleared, no overdraft from a grant
  alone (it stops at $0 with the "used up" message); expiry ($25 granted, $10 spent, $15 stops
  counting, paid money whole), never against a held read, never paying for usage before the grant;
  revocation of the remainder only, once; the allocation order; an account on a grant alone
  invoiced as paid from its balance.
- `MemoryBilling.BalanceGrantsFollowTheContract` and `PostgresBilling.BalanceGrantsFollowTheContract`:
  migration `0100_balance_grants` and its guard, a grant never setting `first_top_up_at`, the
  charges at the grant, lookup by id, revocation and expiry each once with only the unused part
  forfeited, a person's account by email, and the table's checks.
- `frontend/tests/balance.test.ts`: the banner, the Add funds link, the amounts, the history (credits
  from Orama and their expiry) and the return from Stripe, and the Give credit form's input.
- `Coupon.*` (`tests/test_coupons.cpp`): codes normalized and emails digested per inbox; only the
  operator or the instance administrator creates, lists or disables (the operator and a stranger
  cannot redeem into an account); the create checks, generated codes (16 characters, no look-alikes,
  all different) and the ad link with and without regions; a redemption as an ordinary grant (kind
  `grant`, provider `orama`, reference `coupon:<coupon>:<customer>`, the balance, one email, audited
  `billing.coupon.redeemed` with the code and campaign); a second redemption by the same account and
  by the same inbox on another account refused; the cap, the code's expiry and disabling; new
  accounts only (an older account, a topped-up one and one with charges refused); ten failed
  attempts limiting the person and the client address, and refusals audited with a digest only; an
  interrupted redemption finished once; the campaign report.
- `CouponStore.MemoryFollowsTheContract` and `CouponStore.PostgresFollowsTheContract`: migration
  `0101_coupons` and its guard, the code's uniqueness, every claim refusal, resuming an unfinished
  claim, finishing, the attempts and the report, and the tables' checks.
- `RegionsApex.ACouponCodeGoesToTheChosenRegion` and
  `OrdersApex.TheOrderSheetCarriesACouponCodeToTheRegion`: the picker and the order sheet carry
  `?code=` to the chosen region's host and say so.
- `frontend/tests/coupons.test.ts`: `?code=` kept through sign-up (a day, out of the address, replaced
  by a later link, forgotten once Lake answers, kept over a network failure), and the create form's
  input.

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

## Prices and bills

The price list has at most one default rate per usage dimension and one per dimension/class,
keyed on the names `usage_over_time`
answers ([metering.md](metering.md#dimensions)): `unit_micros` for every `per` units of the
dimension's unit. Amounts are integer millionths of the list's three-letter `currency` (USD). A
dimension without a rate is not charged. There is no fee and nothing is included.

**GPU classes** (lake#643). `gpu` and `vram` may also carry one rate per GPU `class` (`l4`, `a10g`,
`a100`, `h100`, or any lower-case class the operator reports): a price `{dimension, class,
unit_micros, per}` bills that class's usage alone, and the dimension's rate without a class bills
every other class and usage of no known class (a GPU the operator could not name). Without a rate
for the dimension itself, a GPU class without its own rate is not charged.

`model_tokens` also has classes (LSR-095, lake#961): the exact requested model followed by
`/input` or `/output`. Each direction has its own positive rate; a blended rate alone cannot
entitle a gateway call. External `record_model_usage` reports also need both prices before they
are recorded, so an untrusted report cannot insert an unpriced class that blocks invoicing.
The [gateway configuration](model-gateway.md#configuration) separately
limits which models may use the platform key, including when billing is off. Other dimensions
reject `class` with `invalid_input`.

A preview or new invoice refuses with `conflict` if a used model/direction class lacks an exact
positive rate in the month's list. It never falls back to the blended rate for those classes,
including when a later publication removes every model price. Old usage without a billing class
retains the historical dimension rate; issued invoices retain their frozen lines. The price-list
editor preserves published model/direction rows when another rate is edited; new model rates can
be published through `set_price_list`.

**Retired from the list** (lake#643), **kept as detail** (lake#658). `instances`, `build_compute`
and `run_compute` are still measured and charted as detail of the compute that is billed (how long
services, builds and runs ran), and never billed: `set_price_list` refuses a rate for them
(`invalid_input` at `/prices/<n>/dimension`), `usage_over_time` answers them with `billed: false`
and the Usage page marks their tiles *Detail, not billed*. Compute is priced as CPU, RAM, GPU and GPU
memory. A list published before lake#643 keeps its rates for them, so a month billed at it still
bills them, and every invoice keeps its lines as issued.

**A workspace engine's disk is billed once, as its volume** (decision 61, lake#663). The engine's
volume is billed as `volumes` by its requested size. What the engine holds on that volume (its
datasets, models, indexes, run artifacts, topics, files and own buckets, from Zetta's
`storage_usage`) is the dimension `engine_storage`: measured, charted, `billed: false`, and marked
*Detail, billed as the engine's volume* on the Usage page. `set_price_list` refuses a rate for it.
`storage` is billed only for what Lake holds outside an engine volume: repositories, container
images, transcripts and drive files. Before this change those engine bytes were billed as `storage`
as well as the volume. From now on, a month's `storage` line counts only what Lake holds outside
the engine's volume, whatever list prices it.

The bill for a month (`YYYY-MM`, a UTC calendar month) is one `usage` line per rate on the list, in
list order, with `dimension`, `class` (empty but for a class rate), `quantity` (the month's use),
`billable` (what is charged; `included` is always 0), the rate (`unit_micros`, `per`) and
`amount_micros` = ⌈`billable` × `unit_micros` ÷ `per`⌉, capped at the largest bigint. The total is
the sum of the lines. A class rate's line is described "GPU (H100)"; the dimension's own rate, when
the list also rates classes, "GPU, other classes", and its quantity is what no class rate covers.

**Time is billed by the minute on the month's total** (lake#637, lake#643, `billable_quantity` in
`include/lake/api/billing.hpp`). Usage is metered by the second; `billable` is the month's summed
usage rounded up once to a whole minute of its unit:

| Unit | Dimensions | Rounded up to |
|---|---|---|
| `seconds`, `instance_seconds`, `gpu_seconds` | `speech`, `build_compute`, `run_compute`, `instances`, `gpu` | a minute (60) |
| `mcpu_seconds` | `cpu` | a vCPU-minute (60,000 thousandths of a vCPU for a second) |
| `mib_seconds` | `ram`, `vram` | a GiB-minute (61,440 MiB-seconds) |

A job is never rounded on its own: a hundred one-second builds are 100 seconds, billed as 2 minutes,
not 100 minutes, and half a vCPU for 61 seconds is billed as one vCPU-minute. Every other
dimension's `billable` is its `quantity`.

`quantity` for a counted dimension is the month's total across the account's live workspaces (a
class rate's, that class's total, from `usage_over_time`'s `by_class`); for
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

Lake does not prorate by day: a whole month has one rate per dimension/class, which keeps a bill
explainable as usage × rate. To raise prices without touching a month already under way, publish
the new list for the next month.

### How the list is stored

Each publication is one `billing_plans` row (name `Usage`, `base_micros` 0, every price's `included`
0) with its rates in `billing_plan_prices` (keyed by dimension and `class` since migration
`0075_workload_resources`, lake#643), under the id
`usage-<YYYYMM>-<publish time in µs, 17 digits>`. The id sorts by the month the list applies from,
then by when it was published, which is the rule above (`price_list_for`,
`include/lake/api/billing.hpp`). Every publication is kept, so an invoice's `price_list_id` always
names the rates it was billed at. Rows with any other id (plans written before lake#635) are
ignored.

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

## Capabilities

The prepaid balance's five capabilities are in [Balance capabilities](#balance-capabilities).

| Capability | Kind | Who | Input | What it does |
|---|---|---|---|---|
| `price_list` | read | anyone signed in | `period?` (`YYYY-MM`; default this month), `include_history?` | `{period, list, next, history?}`: the list in force for the month (null before one is published), a list published for a later month (or null), and, for the operator with `include_history`, every publication, newest first. A list is `{id, effective_from, published_at, currency, prices}`, each price `{dimension, class, title, unit, unit_micros, per, priced}` (`class` empty but for a GPU class rate; `priced` false for a dimension a list from before lake#643 rated and a new list may not). |
| `set_price_list` | action | operator, instance administrator | `prices` (each `{dimension, class?, unit_micros, per?}`), `currency?` (default `USD`), `effective_from?` (`YYYY-MM`; default this month) | Publishes the price list, kept as a new publication. A dimension (or a dimension's class) rated twice, an unknown dimension, a retired one (`instances`, `build_compute`, `run_compute`), a `class` on a dimension without classes, `included` or a fee is `invalid_input`. Audit object `billing_price_list/<id>`, kind `billing.price_list.set`. |
| `billing_account` | read | `read` on the customer | `customer_id` | `{customer_id, month, price_list, provider, contact, suggested_email}`: the list the account is billed at this month (null before one is published), the payment `provider`, the [billing contact](#billing-contact) (null while there is none) and, until one is saved, the signed-in owner's or admin's email to offer. |
| `set_billing_contact` | action | `admin` on the customer (owners, admins), operator | `customer_id`, `email`, `name?`, `line1?`, `line2?`, `city?`, `postal_code?`, `state?`, `country?` | Replaces the [billing contact](#billing-contact) and sends it to the account's Stripe customer, if it has one: `{customer_id, contact, provider_update, provider_error?}`, `provider_update` `updated`, `none` or `failed` (the contact stays saved). |
| `bill_preview` | read | `read` on the customer | `customer_id`, `period?` (`YYYY-MM`; default this month) | The bill for the month worked out now at the list in force for it; nothing is stored. `price_list` is `{id, effective_from}`. `complete` is false when a workspace's usage could not be read (`unread_workspaces`); `not_measured` repeats what `usage_over_time` cannot include; `invoice` is the month's issued invoice that is not void, or null. No list for the month is `conflict`. |
| `issue_invoice` | action | operator, instance administrator | `customer_id`, `period` | Issues the invoice for a month that has ended (`conflict` before), at the list in force for it, with the lines frozen, then asks the provider for payment and, once the invoice is stored and the account has a billing contact, to [email it](#emailing-invoices); a failed send leaves it issued with `provider_status` `send_failed`. Refused with `store_unavailable` while any workspace's usage cannot be read, never issued short; `conflict` when the month already has an invoice that is not void. Issued by hand today; automatic monthly issue is [lake#621](https://github.com/oramasearch/lake/issues/621). |
| `resend_invoice` | action | operator, instance administrator | `invoice_id` | [Emails](#emailing-invoices) a due invoice to the billing email again, with a key of its own; a send moves `send_failed` back to the provider's status. `conflict` when the invoice is not due, has no provider request (manual) or the account has no billing contact; `store_unavailable` with the provider's message when it refuses, leaving the invoice `send_failed`. |
| `list_invoices` | read | `read` on the customer | `customer_id` | The account's invoices, newest month first, each with its lines. |
| `get_invoice` | read | `read` on the customer | `customer_id`, `invoice_id` | One invoice; `not_found` when it is not this account's. |
| `record_invoice_payment` | action | operator, instance administrator | `invoice_id`, `reference?`, `note?` | Marks a due invoice `paid` with the payment's reference. `conflict` when it is not due, or its provider reports payments itself (a Stripe invoice). |
| `void_invoice` | action | operator, instance administrator | `invoice_id`, `note` | Marks a due invoice `void`, after voiding it at its provider (Stripe); the month can be invoiced again. A paid invoice is never voided (`conflict`). |

The operator-only capabilities answer anyone else `unauthorized` before the handler runs. Audit
objects are `customer/<id>/billing` (kinds `billing.account.read`, `billing.contact.set`,
`billing.bill.preview`,
`billing.invoice.issue`, `billing.invoice.send`, `billing.invoice.send_failed`,
`billing.invoices.read`, `billing.invoice.read`, `billing.invoice.paid`, `billing.invoice.void`, and
`billing.account.provider_customer_replaced` beside an issue that replaced the account's Stripe
customer) except `set_price_list`'s.

An invoice is `{id, number, customer_id, period, price_list_id, currency, total_micros, state,
provider, provider_reference, instructions, note, issued_at, paid_at, voided_at, payment_url,
provider_status, lines}`; `state` is `due`, `paid` or `void`, `provider_status` the provider's own
(`open`, `paid`, `payment_failed`, `void`) or `send_failed`, and `number` increases across the
instance. A line is `{kind, dimension, class, description, unit, quantity, included, billable,
unit_micros, per, amount_micros}`, with `kind` `usage`, `class` the GPU class a class rate bills
(empty otherwise, and on every line issued before lake#643), `included` 0, and `billable` the
`quantity` charged (time rounded up to a whole minute of its unit on the month's total). An invoice
issued before lake#643 keeps its `instances`, `build_compute` and `run_compute` lines as issued.

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

A Stripe invoice whose email failed (`provider_status` `send_failed`) is sent again:

```http
POST /v1/resend_invoice
{"invoice_id": "<invoice id>"}
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
- `UsageContract.EveryBilledDimensionHasARateOnThePublishedList` (`tests/test_usage_contract.cpp`,
  lake#658): every dimension the list may rate has a rate in the payload [above](#publish-the-price-list)
  and in the web app's `LIST_RATES`, and the payload rates nothing else.
- `BillLines.ComputeIsBilledByTheVcpuGibAndGpuMinute` and
  `Billing.ComputeIsPricedAsCpuRamGpuAndGpuMemory` (lake#643): CPU, RAM, GPU and GPU memory rounded
  up once to a whole vCPU-, GiB- and GPU-minute; a GPU class at its own rate, the rest at the
  default rate, nothing for an unrated class without one; the retired dimensions and a class on CPU
  refused; a bill from two workspaces' compute and an L4; and an invoice issued at a list from before
  lake#643 keeping its `instances` line after the new list is published.
- `StripeSignature.*`, `StripeClient.*` and `StripeBilling.*` (`tests/test_stripe.cpp`, a fake
  Stripe): the signature with its tolerance window (bad, stale, future, several `v1`); forms and
  cents; a customer made once and kept; an invoice with its items, finalize and hosted URL, every
  write with its own idempotency key; a retry after a failed step that bills once; Stripe's total
  equal to lake's to the cent; a void in lake voiding in Stripe; webhooks applying paid, failed and
  void once per event id, refusing bad and stale signatures, acknowledging unknown events, with the
  central audit; the customer, the invoice and its items tagged `orama_app` and `lake_instance` and
  no Cloud v1 bare key sent (lake#855); the invoice described "Lake usage, `<month>`" and each item
  of `lake_usage` at an inline price with its `-p` key, a negative rounding line keeping the bare
  amount and a positive one the product; a missing product failing the issue with the operator's
  message, nothing issued, and the same issue going through once the product exists; Cloud v1's, untagged and another Lake's invoices, subscription invoices
  included, ignored with no `billing_payment_events` row and no audit event, after the signature
  check, while a Lake-tagged invoice lake did not issue stays `matched: false`; the keys from the
  environment: unset, half set and malformed falling back to manual with a reason that names the
  variable in `billing_account`, `viewer` and `server_info`, never a value, and rotation by restart
  (lake#628); and the route answering only where the lake bills.
- `Billing.OwnersAndAdminsSetTheBillingContact` (`tests/test_billing.cpp`, lake#857): an
  organization account's owner offered their own email, its member and the operator not; the member
  refused, the owner's contact trimmed and shown to the member, an admin's save replacing it whole,
  each refused field `invalid_input` at its path with nothing changed, and the audit naming the
  fields, never their values. `StripeBilling.TheBillingContactReachesTheStripeCustomer`
  (`tests/test_stripe.cpp`): no Stripe call before the first invoice, the customer created with
  the contact's email, name and address, a later save updating it with every address field and its
  own key, a bare contact sending the account's name and `address=`, and a refused update reported
  with the contact kept. `StripeBilling.AnInvoiceIsSentToTheBillingEmailOnceLakeHasStoredIt` and
  `StripeBilling.AFailedSendLeavesTheInvoiceIssuedAndResendSendsIt`: no `/send` without a billing
  contact, the send made only once lake's invoice is stored, after the finalize, under
  `-send`; a refused send leaving the invoice issued, `send_failed` and audited beside the issue;
  `resend_invoice` refused again, then sending it with a key of its own each time and moving it
  back to `open`; a reminder after a failed payment keeping `payment_failed`; a payment applied
  during a failed send kept; and a paid invoice not resent.
  `Billing.AMonthsBillIsWorkedOutFromTheUsageOfEveryWorkspace` adds the owner refused
  `resend_invoice` and a manual invoice's `conflict`. `MemoryBilling.BillingContactsFollowTheContract` and
  `PostgresBilling.BillingContactsFollowTheContract` (`tests/test_postgres.cpp`): the migration's
  table and guard, set, read, replace, refusals, lengths in characters, the table's checks, and the
  cascade with the account.
- `PostgresBilling.PriceListsAndInvoicesFollowTheContract`,
  `PostgresBilling.PaymentProviderCustomersAndNoticesFollowTheContract` and
  `PostgresBilling.WorkloadResourcesAndClassRatesFollowTheContract` (`tests/test_postgres.cpp`):
  the migrations' tables and guards (`0073`, `0074`, `0075`), the price key of plan, dimension and
  class, `set_invoice_provider_status` moving only a due invoice from the status read (lake#857),
  and the store's contract on PostgreSQL and in memory.
- `SchemaConformance.EveryToolAndRequiredOutputUsesHttpContract` calls every billing tool over MCP
  and REST with billing on; `AuditRouting.EveryRegisteredCapabilityIsClassified` checks each is
  central.

## Change

- A new dimension priced: add it to `usage_dimensions()` ([metering.md](metering.md#dimensions)), to
  `DIMENSIONS` and `LIST_RATES` in `frontend/src/pages/billing/billing.ts`, to the [rates](#rates)
  and the payload [above](#publish-the-price-list); `set_price_list` accepts it at once, and
  `UsageContract.*` fails until all of them carry it. A dimension no longer priced: `priced = false`
  in `usage_dimensions()`; it becomes detail (`billed: false`).
- A new GPU class: publish a rate for it (`class`); the editor offers `GPU_CLASSES` and any class a
  published list names. The operator names classes from the node's GPU product label.
- New rates: publish a new list, normally for the next month, and update the [rates](#rates) table
  and its references here.
- Another payment provider: implement `PaymentProvider` against the provider's HTTP API, choose it
  in `PaymentProviders` (by which secret the mount holds, say), record the choice as a decision, and
  document how its payment notices reach lake. Stripe's choice is decision 57; revisit it if fees,
  regions or an enterprise install's own processor call for another.
- A Stripe event lake should act on: map it in `parse_stripe_event` (`src/api/stripe.cpp`), decide
  whose its object is (`foreign_object`, with the `lake_*` key lake sets on it), add the transition
  to `apply_payment_event` in both stores, and register it in the Stripe dashboard.
- A new billing contact field: a migration adding the column with its check, then
  `BillingContact` and `billing_contact_problem` (`include/lake/control/store.hpp`,
  `src/control/memory_store.cpp`), both stores, `contact_json` and the input of
  `set_billing_contact` (`src/api/billing_capabilities.cpp`), `billing_details`
  (`src/api/stripe.cpp`) when Stripe should have it, and the [table above](#billing-contact).
- A new kind of Stripe object lake creates: tag it with `tag` in `src/api/stripe.cpp`
  (`orama_app`, `lake_instance`) and give it a `lake_*` key, never one of Cloud v1's bare keys.
- A capability that starts paid work: add it to `paid_work_capabilities()` (`src/api/balance.cpp`)
  and the [gate's list](#the-gate); one that only looks like it does goes in `free_to_start` in
  `tests/test_balance.cpp`, with why. `PaidWorkContract.*` fails until one of them has it.
- Another balance threshold or amount: an environment variable in `balance_settings_from_environment`
  and `docs/serving/configuration.md`; a per-account one: a column on `account_balances` in a new
  migration, `AccountBalance`, both stores and `account_balance_problem`.
- A new product (a new paid offer, say): add its id to `include/lake/api/stripe.hpp` (and
  `stripe_offer_product` for an offer), the [table above](#stripe), and orama-operator's catalog in
  the same change, so every account has it before a Lake names it.
