# Billing pages

Organization → Billing and Administration → Billing (lake#564, decision 56): this month's usage and
its cost as it grows and the invoice history, one invoice, and the operator's price list editor.
Lake is priced by usage only (lake#635): one rate per usage dimension (and per GPU class for GPU and
GPU memory, lake#643), no plans, no monthly fee and nothing included, so there are no plan cards and
no plan picker. Compute is CPU, RAM, GPU and GPU memory, each at its own rate. They call only the billing
capabilities ([`../api/billing.md`](../api/billing.md)).

## Only where the lake bills

Every billing screen and rail item carries `billing: true` (`frontend/src/pages/billing/routes.ts`,
`routes.tsx`). `billingVisible` (`frontend/src/routes/nav.ts`) shows them only when `viewer.billing`
is true, which the server answers from whether the billing capabilities are registered
(`LAKE_BILLING`). On an enterprise install the rail has no Billing item, the palette offers no
billing page, and opening `/billing` directly answers **Not Available** ("This lake does not bill.
Usage is tracked as usual.") with a link to Usage. Usage pages are unchanged everywhere.

## The pages

| Path | File | What it shows | Capabilities |
|---|---|---|---|
| `/billing?customer_id=` | `billing/BillingPage.tsx` | Figures (this month so far, due, payment), **This Month So Far** (one line per rate: Usage, such as "CPU", "GPU (L4)" or "GPU, other classes", Used (with "billed as" when usage is rounded up to a whole minute, vCPU-minute, GiB-minute or GPU-minute on the month's total, lake#637, lake#643), Rate (per minute, vCPU-minute, GiB-minute or GPU-minute), Cost, where the cost's tooltip reads it as usage × rate = cost; the running total; a warning naming any workspace whose usage could not be read), **No Price List Yet** before one is published, **New Rates From <month>** when a list is published for a later month, **Invoices** (month, number, total, state) and **Not Measured Yet**. The account is the current workspace's; the operator passes `customer_id` and has **Price list** and **Issue an invoice** (a month that has ended). | `billing_account`, `price_list`, `bill_preview`, `list_invoices`, `issue_invoice` |
| `/billing/invoices/:invoiceId?customer_id=` | `billing/InvoicePage.tsx` | One invoice: its lines as issued (usage, used, rate, cost; an invoice from before lake#643 keeps its instance and build and run time lines) and total, a side card (number, month, issued, state, paid or voided, reference, and **Stripe**: Stripe's state of it), **Pay with Stripe** (Stripe's hosted invoice page, `payment_url`) and **How to Pay** while due, **The Last Payment Failed** after a failed payment, the note, and **Print**. The operator has **Void** (why; voids it in Stripe too) on a due invoice, and **Record payment** (reference, note) only on an invoice paid outside lake (provider `manual`). | `get_invoice`, `record_invoice_payment`, `void_invoice` |
| `/billing/plans` | `billing/PriceListPage.tsx` | The operator's price list editor. **Rates** has one row per usage dimension, CPU, RAM, GPU and GPU memory first, and for GPU and GPU memory one more row per GPU class (L4, A10G, A100, H100, and any class the published list names; `rowTitle`: "GPU (H100)", and "GPU, other classes" for the dimension's own rate), each with its rate in the currency, per how many units (`per` in thousandths of a vCPU × seconds, MiB × seconds or GPU × seconds) and the rate as a person reads it ("$0.0015 per vCPU-minute"); an empty rate is not charged. Pre-filled from the list in force or, before one is published, Orama Cloud's list rates (`LIST_RATES`); **From month** (this month by default) and currency; **Publish from <month>** and **Reset to the list rates**. **Publications** lists every list published, newest first, marking the one in force and the next. A note says Metis is priced by sales and not on this list. | `price_list` (`include_history`), `set_price_list` |

The **Payment** figure on `/billing` names the provider (`billing_account.provider`): **Stripe**, "pay
each invoice on Stripe's page", or **Invoice only**. When the manual provider is in use because
Stripe's keys are not set on the platform lake runs on, an **Invoice Only** banner says so: the operator sees
the reason (`provider.reason`), an account sees "Card payment is not set up on this lake yet"
(lake#620). `payOnline` shows **Pay with Stripe** only for a due invoice with an `https://` page;
`recordedByHand`, `providerStatus` and `paymentSummary` are the other rules, in `billing.ts`, with
`lineSentence` (usage × rate = cost), `billedNote` and `perLabel` (time per minute; CPU per
vCPU-minute, RAM and GPU memory per GiB-minute, GPU per GPU-minute), `rateRows`, `rowTitle` and
`ratesInput` (the editor's rows to and from `set_price_list`'s prices, a class on a GPU class row).
The retired dimensions (instances, build and run time) are not in `DIMENSIONS`: republishing a list
from before lake#643 drops their rates.

Amounts are millionths of the currency in the API; the pages show them to the cent, keeping
fractions of a cent only under one unit (a price of $0.0004 a call). Quantities use Usage's units
(`formatValue`). The pure rules are `frontend/src/pages/billing/billing.ts`, tested by
`frontend/tests/billing.test.ts`.

## Words

Bills, rates and invoices say **account** and **workspace**, never space, and never plan. Headings are Title Case;
buttons sentence case. Prices never appear on the Usage dashboard; Billing links to Usage and back.

## Verify

`npx tsc --noEmit -p .`, `npm test` and `npm run build` in `frontend/`. With a lake started with
`LAKE_BILLING=on`, sign in as the operator, open Administration → Billing, publish the list rates,
open `/billing?customer_id=<id>` and see this month's lines and total, publish a list for next
month and see **New Rates From**, and issue last month's invoice; with
`LAKE_BILLING` unset, the rail has no Billing item and `/billing` is Not Available.
