# Billing pages

Organization → Billing and Administration → Billing (lake#564, decision 56): an account's plan, this
month's bill as it grows, plan selection and the invoice history, one invoice, and the operator's
price list. They call only the billing capabilities ([`../api/billing.md`](../api/billing.md)).

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
| `/billing?customer_id=` | `billing/BillingPage.tsx` | Figures (plan, this month so far, due, payment), **This Month So Far** (the bill's lines and total, a warning naming any workspace whose usage could not be read), **Choose a Plan** (a card per plan with its fee and prices; **Choose <plan>** confirms what the choice does: start now, change next month, or stay and cancel a change), **Invoices** (month, number, total, state) and **Not Measured Yet**. The account is the current workspace's; the operator passes `customer_id` and has **Issue an invoice** (a month that has ended). | `billing_account`, `billing_plans`, `bill_preview`, `list_invoices`, `choose_billing_plan`, `issue_invoice` |
| `/billing/invoices/:invoiceId?customer_id=` | `billing/InvoicePage.tsx` | One invoice: its lines as issued and total, a side card (number, month, plan, issued, state, paid or voided, reference), **How to Pay** while due, the note, and **Print**. The operator has **Record payment** (reference, note) and **Void** (why), both on a due invoice. | `get_invoice`, `record_invoice_payment`, `void_invoice` |
| `/billing/plans` | `billing/PlansPage.tsx` | The operator's price list, retired plans included: fee, usage prices with what is included, offered or retired. **New plan** and each plan's name open a drawer: id (fixed once saved), name, description, monthly fee, currency, order, offered, and a price row per dimension (price in the currency, per how many units, included a month). | `billing_plans` (`include_retired`), `set_billing_plan` |

Amounts are millionths of the currency in the API; the pages show them to the cent, keeping
fractions of a cent only under one unit (a price of $0.0004 a call). Quantities use Usage's units
(`formatValue`). The pure rules are `frontend/src/pages/billing/billing.ts`, tested by
`frontend/tests/billing.test.ts`.

## Words

Plans, bills and invoices say **account** and **workspace**, never space. Headings are Title Case;
buttons sentence case. Prices never appear on the Usage dashboard; Billing links to Usage and back.

## Verify

`npx tsc --noEmit -p .`, `npm test` and `npm run build` in `frontend/`. With a lake started with
`LAKE_BILLING=on`, sign in as the operator, open Administration → Billing, add a plan, open
`/billing?customer_id=<id>`, choose it as the account and issue last month's invoice; with
`LAKE_BILLING` unset, the rail has no Billing item and `/billing` is Not Available.
