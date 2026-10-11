# Billing and Invoices

**Organization → Billing** is where your account chooses a plan, sees this month's bill as it grows,
and finds its invoices. The bill is worked out from the usage your workspaces already record (see
[Usage and Metering](/docs/usage)): each plan has a monthly fee and, for each kind of usage, an
amount included every month and a price for what goes beyond it.

Billing is on for Orama Cloud. An enterprise install of lake has no billing: this page, the Billing
item and the billing tools are absent there, and usage is tracked as usual.

## Concepts

| Term | What it is |
|---|---|
| **Plan** | A monthly fee and a price list for usage. Orama publishes the plans; your account is on one at a time. |
| **Price** | For one kind of usage (a dimension, as on Usage), an amount for every so many units: "$1.00 per 1,000 calls". |
| **Included** | The units of a dimension each month's fee already covers. Only usage beyond it is charged. |
| **Bill** | One UTC calendar month on the plan in effect on its first day: the fee, then one line per priced dimension with what was used, what is charged and the amount. |
| **Invoice** | A month's bill once Orama issues it, after the month ends. Its lines are kept as issued; later usage never changes it. |
| **State** | An invoice is **due** until it is **paid**, or **void** if Orama cancels it (the month can then be invoiced again). |
| **Storage on a bill** | The average of each day's storage reading over the month, across your workspaces. |

## Choose a Plan

You need `admin` on the account.

1. Open **Organization → Billing**. **Plans** lists every plan with its fee and prices.
2. Press **Choose** and the plan's name (**Choose Team**), read what happens, and confirm.
   - With no plan yet, the plan applies from the start of this month.
   - Otherwise the change starts next month; this month stays on your current plan.
   - Choosing your current plan again cancels a change that has not started.

- REST: `POST /v1/choose_billing_plan` with `{"customer_id": "<customer id>", "plan_id": "team"}`.
- MCP: `choose_billing_plan` with the same input.

The plans themselves:

- REST: `GET /v1/billing_plans`.
- MCP: `billing_plans`.

Your plan this month, the one chosen for next month and every change:

- REST: `GET /v1/billing_account?customer_id=<customer id>`.
- MCP: `billing_account`.

## See This Month's Bill

**This Month So Far** on **Organization → Billing** shows the bill for the month to date: each line
with what was used, what is included, what is charged and its amount, and the total. It reads every
workspace's usage when you open it. If a workspace does not answer, the page says which one and that
the bill is short until it does.

- REST: `GET /v1/bill_preview?customer_id=<customer id>`, or a past month with
  `GET /v1/bill_preview?customer_id=<customer id>&period=2026-09`.
- MCP: `bill_preview` with `{"customer_id": "<customer id>", "period": "2026-09"}`.

`complete` is false when a workspace's usage could not be read, and `invoice` is the invoice
already issued for that month, if there is one.

## Find an Invoice

**Invoices** on **Organization → Billing** lists every invoice, newest month first, with its total
and state. Press a month to open the invoice: its lines, when it was issued and paid, and **How to
Pay** while it is due. **Print** prints it.

- REST: `GET /v1/list_invoices?customer_id=<customer id>`, and one invoice with
  `GET /v1/get_invoice?customer_id=<customer id>&invoice_id=<invoice id>`.
- MCP: `list_invoices`, and `get_invoice` with
  `{"customer_id": "<customer id>", "invoice_id": "<invoice id>"}`.

## Pay an Invoice

Each invoice says how to pay it. Today Orama issues invoices and collects payment outside lake (for
example by bank transfer), then records the payment, and the invoice shows **paid** with its
reference. Card payment through a payment provider is not available yet.

## Limits

- An invoice covers one UTC calendar month and is issued after the month ends.
- A plan change takes effect on the first day of the next month; there is no part-month proration.
- A workspace deleted before its month is invoiced takes its usage with it, so that usage is not
  billed.
- One currency per plan; taxes are not added.

## Permissions

| Action | Who |
|---|---|
| See plans | Anyone signed in |
| See your account's plan, bill and invoices | `read` on the account |
| Choose a plan | `admin` on the account |
| Set prices, issue an invoice, record a payment, void an invoice | The lake operator |

## Troubleshooting

| You see | What it means |
|---|---|
| **Choose a Plan** and no bill | Your account has no plan yet. Choose one; it applies from the start of this month. |
| **Some Usage Could Not Be Read** | A workspace did not answer when the bill was read. Open the page again later; the invoice waits until every workspace answers. |
| No **Billing** item in the rail | This lake does not bill (an enterprise install). Usage is still under **Organization → Usage**. |

## Related Pages

- [Usage and Metering](/docs/usage): the usage a bill is worked out from, and usage limits.
- [Workspaces](/docs/workspaces): usage is kept per workspace.
- [Organizations, Members and Teams](/docs/organizations): who has `admin` on an organization's account.
