# Billing and Invoices

**Organization → Billing** is where your account sees this month's usage and what it costs so far,
and finds its invoices. Lake is priced by usage only: you pay for what your workspaces use, each
kind of usage at one rate. There is no plan to choose, no monthly fee and no free allowance. The bill
is worked out from the usage your workspaces already record (see [Usage and Metering](/docs/usage)).

Billing is on for Orama Cloud. An enterprise install of Lake has no billing: this page, the Billing
item and the billing tools are absent there, and usage is tracked as usual.

Metis is not billed here. It is enterprise software priced by Orama's sales team; talk to sales.

## Concepts

| Term | What it is |
|---|---|
| **Price list** | Orama's rates, one for each kind of usage (a dimension, as on Usage). Every account is billed at it. |
| **Rate** | An amount for every so many units of one dimension: "$2.00 per 1M calls", "$0.0015 per vCPU-minute" of CPU, "$0.04 per 1 GiB" a month of storage. GPUs have a rate per GPU class (L4, A10G, A100, H100). |
| **Compute** | What your workloads are scheduled with, each at its own rate: CPU (vCPU-minutes), RAM (GiB-minutes), GPU (GPU-minutes, by class) and GPU memory (GiB-minutes). A workload is your workspace's engine, a service, an app, an image build or an agent run. |
| **Bill** | One UTC calendar month: one line per dimension with what was used, its rate and its cost (usage × rate), and the total. |
| **Invoice** | A month's bill once Orama issues it, after the month ends. Its lines and rates are kept as issued; later usage or new rates never change it. |
| **State** | An invoice is **due** until it is **paid**, or **void** if Orama cancels it (the month can then be invoiced again). |
| **Stripe** | Where invoices are paid on Orama Cloud: each invoice has a page on Stripe, and Stripe tells Lake when it is paid. |
| **Storage on a bill** | The average of each day's storage reading over the month, across your workspaces. |

## See the Rates

The rates apply to every account; there is nothing to choose or sign up for.

| Usage | Rate |
|---|---|
| CPU | $0.0015 per vCPU-minute ($0.09 per vCPU-hour) |
| RAM | $0.00017 per GiB-minute ($0.0102 per GiB-hour) |
| GPU, NVIDIA L4 | $0.0108 per GPU-minute ($0.648 per hour) |
| GPU, NVIDIA A10G | $0.0215 per GPU-minute ($1.29 per hour) |
| GPU, NVIDIA A100 | $0.03875 per GPU-minute ($2.325 per hour) |
| GPU, NVIDIA H100 | $0.07875 per GPU-minute ($4.725 per hour) |
| GPU, any other class | $0.03875 per GPU-minute |
| GPU memory | $0.00025 per GiB-minute ($0.015 per GiB-hour) |
| Storage | $0.04 per GiB-month |
| Transfer | $0.05 per GiB |
| Speech to text | $0.0072 per minute |
| API and MCP calls | $2.00 per million |
| Model predictions | $0.01 per 1,000 |
| Model tokens | $9.00 per million |
| Service and store volumes | $0.20 per GiB-month |

Compute is what each workload is scheduled with: the CPU and memory its containers request (or
their limit, when they request nothing), its GPUs and their memory, for as long as it runs. A
pipeline run or a training job inside your workspace runs on your workspace's engine, so it is that
engine's CPU and RAM. An image build or an agent job is billed from when it was requested to when it
finished.

Time is billed by the minute on the month's total: your workspaces' usage of each kind is added up
over the month and rounded up once to a whole minute (a vCPU-minute, a GiB-minute, a GPU-minute), so
many short jobs are never each charged a minute.

Invoices from before compute was priced this way keep their lines as issued, such as **Service and
App Instances** or **Build Compute**.

These are Orama Cloud's list rates; the Lake you use answers its own with `price_list`, which is
what you are billed at.

- REST: `GET /v1/price_list`, or the rates for another month with `GET /v1/price_list?period=2026-11`.
- MCP: `price_list`.

`list` is the price list in force for the month, and `next` a list Orama has published for a later
month, if any. The Billing page announces a coming change as **New Rates From** that month.

## See This Month's Bill

**This Month So Far** on **Organization → Billing** shows the bill for the month to date: one line
per kind of usage with what was used, its rate and its cost, and the total so far. It reads every
workspace's usage when you open it. If a workspace does not answer, the page says which one and that
the bill is short until it does.

- REST: `GET /v1/bill_preview?customer_id=<customer id>`, or a past month with
  `GET /v1/bill_preview?customer_id=<customer id>&period=2026-09`.
- MCP: `bill_preview` with `{"customer_id": "<customer id>", "period": "2026-09"}`.

`complete` is false when a workspace's usage could not be read, and `invoice` is the invoice
already issued for that month, if there is one.

Your account's billing at a glance, the price list it is billed at this month and how invoices are
paid:

- REST: `GET /v1/billing_account?customer_id=<customer id>`.
- MCP: `billing_account`.

## Find an Invoice

**Invoices** on **Organization → Billing** lists every invoice, newest month first, with its total
and state. Press a month to open the invoice: its lines, when it was issued and paid, and **How to
Pay** while it is due. **Print** prints it.

- REST: `GET /v1/list_invoices?customer_id=<customer id>`, and one invoice with
  `GET /v1/get_invoice?customer_id=<customer id>&invoice_id=<invoice id>`.
- MCP: `list_invoices`, and `get_invoice` with
  `{"customer_id": "<customer id>", "invoice_id": "<invoice id>"}`.

## Pay an Invoice

Orama Cloud takes payment through Stripe.

1. Open the invoice from **Invoices** on **Organization → Billing**.
2. Press **Pay with Stripe**. Stripe's invoice page opens, with the same lines and total.
3. Pay by card or bank account there.
4. Come back to the invoice. It shows **paid** once Stripe tells Lake, usually within a minute.

The **Stripe** row on the invoice says where the payment stands: **Awaiting payment**, **Payment
failed** (pay again on Stripe's page with another card or account), **Paid** or **Void**.

If the Billing page's **Payment** figure says **Invoice only**, card payment is not set up on this
Lake. Setting it up is Orama's: Stripe's keys are settings of the platform Lake runs on, never
secrets in your workspace. Orama then sends payment details with each invoice (**How to Pay**), collects the payment
outside Lake, and records it; the invoice shows **paid** with the payment's reference.

The invoice, through the API, carries the same: `payment_url` (Stripe's page) and `provider_status`.

- REST: `GET /v1/get_invoice?customer_id=<customer id>&invoice_id=<invoice id>`.
- MCP: `get_invoice` with `{"customer_id": "<customer id>", "invoice_id": "<invoice id>"}`.

## Limits

- An invoice covers one UTC calendar month and is issued after the month ends.
- A month is billed at the rates in force at its end. New rates apply from the first day of a month;
  rates published during a month price the whole of it. There is no part-month proration.
- A workspace deleted before its month is invoiced takes its usage with it, so that usage is not
  billed.
- Prices are in US dollars; taxes are not added.

## Permissions

| Action | Who |
|---|---|
| See the price list | Anyone signed in |
| See your account's bill and invoices | `read` on the account |
| Publish the price list, issue an invoice, record a payment, void an invoice | The Lake operator |
| Pay an invoice on Stripe | Anyone with the invoice's Stripe page |

## Troubleshooting

| You see | What it means |
|---|---|
| **No Price List Yet** and no bill | Orama has not published its rates on this Lake. Your usage is recorded; nothing is billed until the rates are published. |
| **Some Usage Could Not Be Read** | A workspace did not answer when the bill was read. Open the page again later; the invoice waits until every workspace answers. |
| No **Billing** item in the rail | This Lake does not bill (an enterprise install). Usage is still under **Organization → Usage**. |
| No **Pay with Stripe** on a due invoice | Card payment is not set up on this Lake (**Payment** says **Invoice only**), or the invoice was issued before it was. Follow **How to Pay**. |
| Paid on Stripe, still **due** in Lake | Stripe's notice has not arrived yet. Open the invoice again in a minute; Stripe keeps retrying until Lake answers. |

## Related Pages

- [Usage and Metering](/docs/usage): the usage a bill is worked out from, and usage limits.
- [Workspaces](/docs/workspaces): usage is kept per workspace.
- [Organizations, Members and Teams](/docs/organizations): who has `admin` on an organization's account.
