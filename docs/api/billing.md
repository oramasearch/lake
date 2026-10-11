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

**No payment provider has been chosen** ([lake#620](https://github.com/oramasearch/lake/issues/620),
a decision still to make). Lake collects payment through
`PaymentProvider` (`include/lake/api/billing.hpp`), an interface with:

- `name()` and `title()`: stored on each invoice (`provider`) and shown by `billing_account`;
- `request_payment(invoice)`: called for an invoice about to be issued (its account, month, plan,
  lines and total). It answers the provider's `reference` and the `instructions` the invoice shows,
  or an error, which leaves the invoice unissued (`store_unavailable`);
- `records_payments_by_hand()`: whether the operator records each payment with
  `record_invoice_payment` (true for the manual provider; a provider that reports payments itself
  answers false, and `record_invoice_payment` then answers `conflict`).

Lake ships one implementation, **manual** ("Invoice only"): lake issues the invoice, Orama collects
payment outside lake, and the operator records it. A real provider is lake's own code against the
provider's HTTP API through lake's outbound client, never a third-party SDK (liquid-bridge
`CLAUDE.md`, "We write our own"). Tests inject a fake through `register_billing_capabilities`.

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
| `record_invoice_payment` | action | operator, instance administrator | `invoice_id`, `reference?`, `note?` | Marks a due invoice `paid` with the payment's reference. `conflict` when it is not due, or the provider reports payments itself. |
| `void_invoice` | action | operator, instance administrator | `invoice_id`, `note` | Marks a due invoice `void`; the month can be invoiced again. A paid invoice is never voided (`conflict`). |

The operator-only capabilities answer anyone else `unauthorized` before the handler runs. Audit
objects are `customer/<id>/billing` (kinds `billing.account.read`, `billing.plan.choose`,
`billing.bill.preview`, `billing.invoice.issue`, `billing.invoices.read`, `billing.invoice.read`,
`billing.invoice.paid`, `billing.invoice.void`) except `set_billing_plan`'s.

An invoice is `{id, number, customer_id, period, plan_id, plan_name, currency, total_micros, state,
provider, provider_reference, instructions, note, issued_at, paid_at, voided_at, lines}`; `state` is
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
- `PostgresBilling.PlansSubscriptionsAndInvoicesFollowTheContract` (`tests/test_postgres.cpp`): the
  migration's tables and guard, and the store's contract on PostgreSQL.
- `SchemaConformance.EveryToolAndRequiredOutputUsesHttpContract` calls every billing tool over MCP
  and REST with billing on; `AuditRouting.EveryRegisteredCapabilityIsClassified` checks each is
  central.

## Change

- A new dimension priced: add it to `usage_dimensions()` ([metering.md](metering.md#dimensions));
  `set_billing_plan` accepts it at once.
- A payment provider: implement `PaymentProvider` against the provider's HTTP API, pass it as
  `ServerOptions::payment_provider`, record the choice as a decision, and document how its payment
  notices reach lake.
