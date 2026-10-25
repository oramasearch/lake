# Orders: the Apex Order Sheet

lake#831. Steve, 2026-10-18: *"this picker here could actually be a sales order thing"*, *"this is
basically an order sheet. Make it convenient to order"*, and *"this should be hooked into stripe and
will deploy with the mgmt server (operator) upon successful purchase ... please allow a user to
submit an order form for a larger order and skip paying if they want to ... (log it to controldb
postgres and send an email to support@orama.com with resend vendor)"*.

The page itself is described in [`../ui/regions.md`](../ui/regions.md#the-order-sheet); the region
choice it keeps is [`regions.md`](regions.md#the-apex-picker). Provisioning paid orders is the
Orama operator's half: https://github.com/oramasearch/orama-operator/issues/634.

## What exists

The apex host (`LAKE_APEX_HOST`, `lake.live`) shows every way to get Lake, with its price in USD:

| Offer | Price (default) | Included usage credit | How it is ordered |
|---|---|---|---|
| Shared, in a region that runs today | Usage-based, at the published price list | — | Continue to the region's host, exactly as the picker did |
| Private warehouse (`private_warehouse`): a dedicated warehouse on shared hardware, in a shared region | $1,000 / month | $400 (40%) | Stripe Checkout, or the order form |
| Dedicated cluster (`dedicated_cluster`): a Kubernetes cluster on the provider and in the region the customer chooses | $5,000 / month | $2,000 (40%) | Stripe Checkout (providers the operator provisions), or the order form |
| On-prem (`on_prem`): source available, full service (Metis), SOC 2 compliant; any provider or bare metal, any accelerators, any region, multi-region with geo-based load balancing and failover | From $10,000 / month | 40% of the fee | The order form only |

Every offer says backups are included. Usage beyond the credit is billed at the published price
list; how a bill applies the credit is in [billing.md](billing.md#order-sheet-subscriptions).

**Your own domain, white-labelled.** Private Warehouse and every offer above it (Dedicated
Cluster, On-Prem) list *"Your own domain, white-labelled (available on request)"* as a feature;
the shared option does not (lake#1129, Steve, 2026-10-24). A customer on one of those offers can
have their Lake serve their own brand at their own address (for example `data.example.com`)
instead of `lake.live`. It is a line of plan data in each offer's `features`, not page copy, and it
says *on request* because it is arranged with support, not self-serve: Lake has custom domains for
**applications** today (`create_domain`), not yet for a whole warehouse. What a white-labelled
warehouse still needs (the customer's domain and TLS on a zone they own, branding with no Orama or
Lake marks, sign-in on their domain, the agent surfaces on their domain) is filed and linked from
https://github.com/oramasearch/lake/issues/1129. Drop *"(available on request)"* only when that is
delivered.

**Two paths, one table.** Every order is a row of `cloud_orders` in lake's control-plane
PostgreSQL (orders are multitenancy and billing, decision 56; nothing from a workspace is in it):

- **Pay** (private warehouse, dedicated cluster in a `checkout` region of a `checkout` provider): the order is recorded
  `pending_payment`, lake creates a Stripe Checkout Session and sends the person there. Stripe's
  signed webhook marks it `paid`. The operator provisions `paid` orders and writes `provisioning`,
  then `ready` with the host, or `failed` with the reason. A checkout that expires or whose payment
  fails is `cancelled`; nothing is provisioned.
- **Order form** (every offer; the only path for on-prem and for anything not listed): the order
  is recorded `submitted` and emailed to support and to the customer. It is never provisioned.

```
submitted                                   (order form: emailed, never provisioned)
pending_payment ──Stripe──▶ paid ──operator──▶ provisioning ──▶ ready | failed
       └──────── expired / payment failed / checkout could not start ──▶ cancelled
```

## Routes

All on the apex host, answered by the region picker middleware before any other route
(`src/api/http_server.cpp`, `serve_region_picker`). Server-rendered, `no-store`, no JavaScript.
They exist while the order sheet is `enabled` and the Lake has a control-plane store; every other
apex path is the sheet, exactly as every path was the picker.

| Route | Answer |
|---|---|
| `GET` any other path | The order sheet; the shared option's form posts back to the same path and query ([regions.md](regions.md#the-apex-picker)) |
| `POST` any other path with `region=<name>` | `302` to `https://<region host><path>?<query>` with the `lake_region` cookie, unchanged |
| `GET /order/private-warehouse`, `/order/dedicated-cluster`, `/order/on-prem` | The offer's order form; `?location=<provider>:<region>`, a shared region name, or `other` preselects where it runs |
| `GET /order/price-list` | The published price list in force this month (the `billing_plans` publication `price_list_for` picks), or a sentence when none is published |
| `POST /order` (`application/x-www-form-urlencoded`, at most 32 KiB) | Fields `offer`, `path` (`checkout` or `order_form`), `location`, `contact_name`, `contact_email`, `company`, `details`, `quantity` and the hidden `website`. Pay: `302` to Stripe Checkout. Order form: `200` with the order id and the status link. Invalid: `400` with the form and each field's error. `413` over 32 KiB, `429` over the limits, `503` when the order cannot be recorded or the checkout cannot start (the order is then `cancelled` and the form offers the order form path) |
| `GET /orders/<id>?token=<token>` | The order's status: its state in words, what was ordered, and the host once `ready`. `404` for a token that does not match. `checkout=cancelled` (Stripe's cancel URL) says the payment was not completed |

`/billing/stripe/webhook` is not on the apex: Stripe's endpoint is the regional host of the Lake
that serves the apex (for production, `https://eu.lake.live/billing/stripe/webhook` while
`LAKE_APEX_HOST` is set on the Europe member), because that Lake's PostgreSQL holds the orders.

### Validation

`validate_order_form` (`src/api/orders.cpp`) checks a form against the configuration:

- `offer` is one of the three paid offers.
- Where it runs: a private warehouse names a shared region (`LAKE_REGIONS`); a dedicated cluster
  names `<provider>:<region>` from the configuration; on-prem says it in `details`. `other` is
  anything not listed and needs `details`.
- `path=checkout` only for an offer with `checkout`, a listed location in a region with `checkout`
  (on a provider with `checkout`), a quantity of 1, and a Lake with card payment (Stripe configured,
  `server_info.online_payment`). Otherwise the order form. A paid order is one warehouse or one
  cluster: the operator provisions one and writes one `host`, so more than one is a larger order
  ("Card payment orders one at a time: send the order form for more.").
- A name (1–200 characters, on one line), an email address, a company (≤ 200), details
  (≤ 4,000, line breaks kept), a quantity from 1 to 1,000 (default 1).
- The amount is the monthly fee times the quantity; the credit is `credit_percent` of it.

### Abuse protection

The apex is anonymous, so the form cannot flood support:

- **Rate limits**: at most `orders_per_client_per_hour` orders (default 5) from one address, and
  `orders_per_hour` (default 60) in all **per path**, over a sliding hour, kept in memory per
  process. Checkout orders and order forms have separate in-all caps (lake#838), so order forms,
  which anyone can send, can never use up the cap a paying customer is counted against. Past a
  limit: `429`, nothing recorded.
- **The address** ([client addresses](../deployment/client-addresses.md), lake#1083), from a
  trusted proxy (`LAKE_TRUSTED_PROXY_CIDRS`), is `X-Real-IP`, which the ingress (ingress-nginx)
  always sets itself, overwriting any a sender wrote; else the last `X-Forwarded-For` hop (the one
  the nearest proxy appended); else the proxy's own address. From any other peer it is the peer's
  address, whatever headers it sent. Lake never reads `CF-Connecting-IP` or the first
  `X-Forwarded-For` hop: a sender that reaches the ingress without passing Cloudflare could write
  either and land in a fresh bucket with each request (lake#838). Which client address to trust is
  the ingress's decision: until it trusts Cloudflare's header from Cloudflare's ranges (https://github.com/oramasearch/orama-operator/issues/719),
  visitors behind one Cloudflare edge share a bucket.
- **Honeypot**: the form has a field named `website` that is off-screen and `aria-hidden`; people
  never fill it. A form that fills it is answered with a confirmation and nothing is recorded or
  sent (`lake_cloud_orders_refused_total{reason="honeypot"}`).
- **Size**: the body is at most 32 KiB, and each field has the limits above, enforced again by the
  schema's checks.

## Configuration

One source for offers, prices, credit, providers and regions: `default_order_sheet_json()` in
`src/api/orders.cpp`, the defaults below, with **`LAKE_ORDER_SHEET`** merged over it as an RFC 7386
JSON merge patch, read and validated once at startup (`order_sheet_from_environment`). An object
key replaces or merges; an array replaces the default's array whole. Nothing on the page is copy
outside it.

```json
{
  "enabled": true,
  "url": "",
  "currency": "USD",
  "shared": {"title": "Shared", "summary": "…", "features": ["…", "Backups included", "…"],
             "price_list_url": "", "promise": "Train and serve a model in 5 minutes for under $5",
             "show_promise": false},
  "offers": {
    "private_warehouse": {"title": "Private Warehouse", "monthly_usd": 1000, "credit_percent": 40,
                          "checkout": true, "starting_from": false, "summary": "…", "features": ["…"]},
    "dedicated_cluster": {"title": "Dedicated Cluster", "monthly_usd": 5000, "credit_percent": 40,
                          "checkout": true, …},
    "on_prem": {"title": "On-Prem", "monthly_usd": 10000, "credit_percent": 40, "checkout": false,
                "starting_from": true, …}
  },
  "providers": [
    {"id": "azure", "label": "Microsoft Azure", "checkout": true, "regions": [
      {"id": "eastus", "label": "United States (Virginia)"}, {"id": "westus2", …},
      {"id": "swedencentral", …}, {"id": "westeurope", …}, {"id": "canadacentral", "label": "Canada (Toronto)"},
      {"id": "japaneast", "label": "Asia (Tokyo)", "checkout": false},
      {"id": "southeastasia", "label": "Asia (Singapore)", "checkout": false}]},
    {"id": "aws", "label": "Amazon Web Services", "checkout": false, "regions": […]},
    {"id": "gcp", "label": "Google Cloud", "checkout": false, "regions": […]}
  ],
  "trust": [
    {"text": "SOC 2 compliant: ask for the report", "url": "mailto:support@orama.com?subject=SOC%202%20report"},
    {"text": "Source available, with access to the source code", "url": "mailto:support@orama.com?subject=Source%20access"}
  ],
  "limits": {"orders_per_client_per_hour": 5, "orders_per_hour": 60}
}
```

- **`enabled`** `false`: the apex is the plain region picker of lake#757 and `/order…` paths are
  picker paths again.
- **`url`**: where the sheet is, for the web app's "New setup…" and "On-prem…" entries
  (`server_info.order_sheet_url`). Empty: `https://<LAKE_APEX_HOST>/` on the Lake that serves the
  apex, and none elsewhere. Set it (`{"url":"https://lake.live/"}`) on the other regional Lakes so
  their web app links back to the sheet.
- **`providers[].checkout`**: whether the operator provisions on that provider. Only Azure is
  `true`, because the operator creates clusters on Azure only today; the others are ordered through
  the order form until it supports them, and the form marks them "(order form)".
- **`providers[].regions[].checkout`**: whether the operator can create a cluster in that region;
  it defaults to the provider's value and cannot be `true` on a provider whose `checkout` is
  `false` (startup stops naming the field). The operator reaches only the Azure regions in its
  `cloudRegionLocations` (orama-operator#635, `internal/controller/lake_regions.go`: canadacentral,
  centralus, eastus, eastus2, francecentral, germanywestcentral, northeurope, southcentralus,
  swedencentral, uksouth, westeurope, westus, westus2, westus3). So the default lists Tokyo
  (`japaneast`) and Singapore (`southeastasia`) with `checkout: false`: they appear in the form as
  "(order form: we'll confirm availability)", and paying for them is refused with "This region is
  ordered with the order form: we will confirm availability." before anything is recorded or sent
  to Stripe. When the operator reaches a new region, set its `checkout` to `true` (or drop the
  override) in the same release.
- **`shared.show_promise`** is **off by default**. The line *"Train and serve a model in 5 minutes
  for under $5"* is shown only while the test that backs it passes and is recorded in
  [billing.md](billing.md#the-shared-tiers-promise); it is not recorded yet, so do not switch it on.
- **`trust`**: each claim is shown only with a link (`https` or `mailto`) that backs it. Change the
  links to the published report and the source-access terms when they exist; only what is true at
  launch is claimed.
- Prices are whole dollars, USD only (the price list and Stripe bill USD). The prices and the 40%
  credit are the call's figures and are among the decisions still open in lake#831 (whether they
  become price-list plans first; the credit's percentage, expiry and on-prem meaning; the region
  list per provider; multi-region pricing).

An invalid value stops startup with exit code 2 and one line naming the field, for example
`lake: LAKE_ORDER_SHEET.offers.private_warehouse.monthly_usd must be between 1 and 100000000`.
The startup log line carries `order_sheet` (`on`/`off`) and `resend_api_key_set` (`yes`/`no`).

## Paying

`POST /order` with `path=checkout`, after the order is recorded `pending_payment`, calls the payment
provider's `create_checkout` (`include/lake/api/billing.hpp`). Stripe's
(`src/api/stripe.cpp`) is `POST /v1/checkout/sessions` with `Idempotency-Key:
lake-order-<id>-checkout`:

| Field | Value |
|---|---|
| `mode` | `subscription` |
| `line_items[0][price_data]` | `currency=usd`, `product` the offer's Lake product (`lake_private_warehouse`, `lake_dedicated_cluster`), `unit_amount` the offer's monthly fee in cents, `recurring[interval]=month` |
| `line_items[0][quantity]` | the order's quantity |
| `subscription_data[description]` | where it runs ("Microsoft Azure, Canada") |
| `client_reference_id`, `metadata[lake_order_id]`, `subscription_data[metadata][lake_order_id]` | the order id |
| `metadata[orama_app]`, `metadata[lake_instance]`, and the same under `subscription_data[metadata]` | `lake` and the Lake's public host, the tags of a shared Stripe account (lake#855, [billing.md](billing.md#stripe)) |
| `customer_email` | the contact email |
| `success_url`, `cancel_url` | the status link with `&checkout=done` / `&checkout=cancelled` |

The price is inline (`price_data`), so the configuration stays the one source of prices; no Stripe
Price is created by hand. The line names the offer's Lake product (lake#855,
[billing.md](billing.md#stripe)), which the operator creates in the account with the name
Checkout shows ("Lake Private Warehouse", "Lake Dedicated Cluster"); where it runs is the
subscription's description. The session id is kept on the order, and the person is sent
to the session's `url`. When the session cannot be created (no Stripe keys, Stripe refuses), the
order is moved to `cancelled` with the reason as its event's detail, and the form says to send it as
an order form. An account without the product refuses with the same message as an invoice, "Stripe
has no product lake_dedicated_cluster in this account; the operator creates it (spec.payments)".

**The webhook** (`POST /billing/stripe/webhook`, [billing.md](billing.md#webhooks)) gains the
Checkout events, matched by the session id and applied once per event id (the same
`billing_payment_events` row an invoice event uses), only from `pending_payment`:

| Event | Order |
|---|---|
| `checkout.session.completed` with `payment_status` `paid` or `no_payment_required` | `paid`, with `stripe_customer_id` and `stripe_subscription_id` |
| `checkout.session.completed` with `payment_status` `unpaid` (a delayed payment method) | unchanged; waits for the next event |
| `checkout.session.async_payment_succeeded` | `paid` |
| `checkout.session.async_payment_failed`, `checkout.session.expired` | `cancelled` |

The answer is `{received, order_id, state, changed}`; a repeat delivery `{received, duplicate}`; a
session tagged as this Lake's that it did not start `{received, matched: false}` (audited as
ignored). A session without `metadata.lake_order_id` (another app's) or whose `lake_instance` names
another Lake is `{received, ignored: true}`, with nothing recorded (lake#855,
[billing.md](billing.md#webhooks)). A late completion for
a `cancelled` order changes nothing. Subscribe the Stripe endpoint to these four events besides the
invoice events. Test mode only until lake#754 (Stripe live).

## Email

Through **Resend** (`include/lake/api/resend.hpp`, `src/api/resend.cpp`): lake's own client,
`POST https://api.resend.com/emails` with `Authorization: Bearer <LAKE_RESEND_API_KEY>`,
`Idempotency-Key: lake-order-<id>-<state>-<recipient>` (Resend answers a repeated key within 24
hours with the first send, so a retry never sends twice) and a JSON body
`{from, to, subject, text, reply_to}`. The key is Orama's platform secret, an environment variable
of the hosting platform (`spec.env` with a `secretKeyRef`), never a workspace secret, never logged;
a 401/403 is reported without any of it.

| State | Support (`LAKE_SUPPORT_EMAIL`) | Customer (the contact email) |
|---|---|---|
| `submitted` | Every field and the order id; reply-to the customer | A copy, with the status link |
| `paid` | Every field and the order id | Payment received, with the status link |
| `ready` | — | Ready, with the host and the status link |
| `failed` | Every field, the order id and the failure reason | A problem, support has been told |

**The customer's copy carries nothing the sender typed.** It goes to whatever address was typed into
an anonymous form, so it must not relay a stranger's words from `orders@lake.live`: no name, company,
details or failure reason, and no greeting with the name. It has Lake's own wording, the offer, where
it runs (the configuration's labels, or "Not listed: as described"), the quantity, the price and
credit, the order id, the status link, and the host once `ready`. Support's email keeps every field,
the free text included (`TheCustomersCopyCarriesNothingTheSenderTyped` in `tests/test_orders.cpp`).

**Delivery state is on the row**: `support_emailed_state` and `customer_emailed_state` (the last
state each recipient was told about), `email_attempts`, `email_error` and `email_retry_at`. The
**order pass** (`run_order_pass`, on the maintenance thread every 10 seconds, sooner after an order
form, and only on the store root's owner) claims orders with an email due (`claim_cloud_order_emails`,
which holds each for five minutes so a second pass never takes it), sends what is due, and records
each outcome. A failure is retried after 1, 2, 4 … minutes, at most an hour apart. An order that
passes through a state quickly (paid, then ready before the pass) is emailed about the state it is
in.

**Without the key** the order is still recorded and the page says it was received; the order's
`email_error` says `LAKE_RESEND_API_KEY is not set`, a `cloud_order_email_waiting` line is logged,
and the pass logs `cloud_order_email_disabled` once. The emails stay due and go out on the first
pass after lake restarts with the key.

The sender is `LAKE_RESEND_FROM` (default `Lake <orders@lake.live>`), whose domain must be verified
in Resend before anything is delivered.

## Status links

`/orders/<id>?token=<token>`, where the token is HMAC-SHA-256 over the order id under
`LAKE_INSTANCE_KEK`, base64url (`order_status_token`, the signed-link helper of
[http.md](../serving/http.md#signed-links)). Only someone given the link (the confirmation page,
the customer's emails, Stripe's return URLs) can read the order. No token is stored. A Lake without
an instance key makes a random key at start, so its links work until it restarts; rotating the
instance key ends every earlier link.

## Audit

Every state change is one `cloud_order_events` row, written in the transaction that makes it:
lake's (`actor` `lake`), Stripe's notices (`stripe`) and the operator's (`operator`). The order
pass records each row not yet recorded in the central audit log and sets its `audited_at`, so a
change is never lost when the audit write fails. The audit event is kind `cloud_order.<to_state>`,
object `cloud_order/<id>`, action `cloud_order_state`, principal `lake`, `payment-provider` or
`operator`, details `{event_id, at, from_state, to_state, actor, detail}`, and no account (orders
are anonymous). It is readable with `audit` filtered by object.

## The table contract

Migration `0089_cloud_orders` ([schema.md](../control-plane/schema.md#cloud-orders-migration)).
Lake writes every column; the operator reads `paid` rows and writes only `state`, `host`,
`failure_reason`, `claimed_by`, `claimed_at` and `cloud_order_events` rows. The trigger keeps
`updated_at` on every write and announces a `paid` order on the `lake_cloud_orders` channel.

## Recurring entitlement (LSR-105, lake#972)

Migration `0097_cloud_order_entitlements` keeps each checkout order's unique Stripe subscription
and customer, `subscription_status`, `paid_through`, `entitlement_access_until`,
`subscription_event_created`, `entitlement_grace_seconds`, and `entitlement_retry_invoice_id`.
Times are UTC Unix seconds.
`paid_through` advances only on a signed paid invoice snapshot (`invoice.paid` or an
`invoice.updated` whose status is `paid`), using the recurring lines' paid period,
not the invoice's billing-period summary. Active and trialing subscription snapshots grant access
until their period or trial ends. A failed payment keeps the last paid period and may extend access
to Stripe's signed `next_payment_attempt` (also read from `invoice.updated` for Stripe Automations).
The first unpaid invoice owns that retry window until positive entitlement evidence resets it;
new monthly unpaid invoices cannot move it forward. Negative events cannot create entitlement or
restore an order that has already expired, including when a grace configuration increases.
PostgreSQL rechecks its own clock while updating the locked row.
An unpaid, paused, incomplete, unknown, or canceled
subscription ends access at the earlier existing deadline or the event's end time. A canceled
subscription cannot be revived; purchasing a new subscription requires a new order.

`LAKE_ORDER_ENTITLEMENT_GRACE_SECONDS` sets the grace while entitled or on positive evidence
(default `259200`, three
days; integer `0` through `2592000`). Invalid configuration stops startup. Subscription grace starts
at the access deadline, so redelivery cannot keep moving it forward. Initial paid Checkout gets
one grace window from receipt while the signed subscription/invoice snapshot arrives; it does not
invent a paid month. Existing orders with no entitlement proof default to suspended until a valid
signed snapshot arrives. No data or order is removed.

The verified Stripe webhook handles `invoice.paid`, `invoice.payment_failed`, `invoice.voided`,
`invoice.marked_uncollectible`, `invoice.payment_action_required`, `invoice.updated`, and
`customer.subscription.{created,updated,deleted,paused,resumed}` for order subscriptions. It accepts
the pinned 2024 invoice `subscription_details` and current `parent.subscription_details`, legacy
subscription periods and current item periods, and string or expanded customer/subscription ids.
An event must match the recorded subscription, customer and order and this Lake's metadata.
Before Checkout completes, a signed event can bind the pending checkout order by its metadata;
a later Checkout cannot replace that binding. Both stores apply an event id once, serialize
updates, ignore stale access changes (while retaining paid-period evidence), prefer denial on
ambiguous same-second updates, and retain
terminal cancellations even if delivered after a newer invoice. Subscription changes write an
order event for the central audit pass. Ordinary usage invoice processing stays separate.

### Operator handoff required before release

Lake supplies **`cloud_order_desired_state`**, a view containing every `cloud_orders` column as of
`0097_cloud_order_entitlements`, named one by one, plus the two below. The columns are listed rather
than `cloud_orders.*`, so a later `ALTER TABLE cloud_orders ADD COLUMN` cannot break applying the
schema again; a later migration that wants a new column in the view appends it after
`desired_state`.

The unique index `cloud_orders_subscription_unique` on `stripe_subscription_id` makes a subscription
belong to one order. Creating it fails, and with it the whole schema apply, if two existing orders
already share a subscription id; such rows must be corrected first. On the existing Lakes every order
is test data.

| Column | Values |
|---|---|
| `entitlement_state` | `active` before the access deadline, `grace` before deadline + grace, otherwise `suspended`; order forms are `not_applicable` |
| `desired_state` | `present` only for checkout orders in `paid`, `provisioning` or `ready` with active/grace entitlement; otherwise `suspended`; order forms are `unmanaged` |

These values use PostgreSQL's statement clock on every read, including when no more webhook arrives.
The operator must vendor `db/schema.sql` byte-for-byte, select `cloud_order_entitlements_guard`,
register the event types above in `lake_payments.go`, and read this view for first provisioning
and every managed checkout order or owned resource, including in-flight, ready, failed and canceled
orders. Do not filter suspended rows out of reconciliation. It must suspend compute and prevent recreation when
`desired_state = 'suspended'`, retain customer data and order/resource identity, and restore compute
when a later signed renewal or active snapshot makes the same order `present`. Recheck entitlement
before provisioning writes so a claim or stale ready row cannot bypass expiry. Fake-store operator
tests must cover initial provisioning, missing-resource recreation, grace expiry, subscription
end, in-flight claims, partial failures, restoration, and data retention. This Lake change supplies the contract; **operator suspension
and webhook registration are not implemented here**, so the issue remains open until that paired
change ships. Do not enable automatic order provisioning on the old operator.

Verify Lake with `ctest --test-dir build --timeout 600 -R 'OrdersEntitlement|OrdersCheckout|OrdersStore'`.
The signed lifecycle runs against both stores; the PostgreSQL view test expires an already-ready
order without another webhook and restores it on renewal. The tests exercise retry grace,
cancellation, stale and duplicate delivery, identity mismatches, trials, configurable grace,
pre-Checkout delivery and invalid signatures. Stripe's field and event contracts are documented in
[subscription webhooks](https://docs.stripe.com/billing/subscriptions/webhooks),
[subscriptions](https://docs.stripe.com/api/subscriptions/object),
[invoices](https://docs.stripe.com/api/invoices/object), and
[retry events](https://docs.stripe.com/billing/revenue-recovery/smart-retries).

## Metrics

`lake_cloud_orders_total{offer,path}` (orders recorded), `lake_cloud_orders_refused_total{reason}`
(`honeypot`, `rate`, `size`) and `lake_cloud_order_emails_total{recipient,outcome}` (`sent`,
`failed`); [telemetry.md](../serving/telemetry.md).

## Not yet

- **Provisioning**: orama-operator#634. Until it ships, a `paid` order stays `paid` and support
  takes it forward from the email.
- **A signed-in customer's own clusters in the picker** needs one identity across regions (lake#758).
- **The credit on the monthly bill** (the credit applied first, the rest as usage lines) is not
  built: [billing.md](billing.md#order-sheet-subscriptions).
- **The 5-minute promise** stays off until its test is recorded.
- **A restore rehearsed on each tier** (orama-operator#127) backs the backups line.
- **Real keys**: Stripe live (lake#754) and a Resend key with a verified sending domain are
  platform secrets still to be set ([../deployment/resend.md](../deployment/resend.md),
  [../deployment/stripe.md](../deployment/stripe.md)).
- `organization_id` is a nullable column for lake#832 (orders and servers belonging to
  organizations); the apex is anonymous and never sets it today.

## How to verify

- `ctest --test-dir build -R 'Orders|Resend'` runs `tests/test_orders.cpp`: the default and the
  merge patch, every startup error, the form's checks for each offer and path, the status token,
  the rate limit, who is emailed about which state and what the emails say, the Resend client
  against a test double for HTTP (headers, body, idempotency key, refusals), the Checkout request
  with each offer's Lake product and a missing product's message,
  the Checkout events, the webhook moving an order once per event, the `cloud_orders` contract in
  the memory store and in PostgreSQL (with orama-operator#635's own statements), and over HTTP the
  sheet, an order form recorded and emailed, a paid order through Checkout, the webhook and
  `ready`, a failed checkout, the honeypot, the rate limit, the size limit, a missing Resend key and
  a retried email.
- By hand, with a local Lake (the apex host set to the loopback address so a browser can open it):

  ```
  LAKE_REGION=swedencentral LAKE_APEX_HOST=127.0.0.1 \
  LAKE_REGIONS='[{"name":"swedencentral","label":"Europe","host":"eu.lake.live","latitude":"60.67488","longitude":"17.14127"},{"name":"eastus","label":"United States","host":"us.lake.live","latitude":"37.3719","longitude":"-79.8164"}]' \
  ./build/lake --memory-store --listen 127.0.0.1:4900
  open http://127.0.0.1:4900/
  curl -s -d 'offer=on_prem&path=order_form&contact_name=Ada&contact_email=ada%40example.com&details=Bare+metal' \
    http://127.0.0.1:4900/order | grep -o 'Order [0-9a-f-]\{36\}'
  ```

## How to change it

- Prices, credit, offers' words, providers, regions, trust links and limits: `LAKE_ORDER_SHEET`
  on the Lake, or the default in `src/api/orders.cpp` with this page.
- The pages: `web_ui_order_sheet`, `web_ui_order_form`, `web_ui_order_received`,
  `web_ui_order_status` and `web_ui_price_list` in `src/api/web_ui.cpp`; styles `.order-*` in
  `src/api/web_ui_styles.inc`.
- A new state or column is a new migration and a change to orama-operator#634's reader in the same
  release.
