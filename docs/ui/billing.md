# Billing pages

Organization → Billing and Administration → Billing (lake#564, decision 56): this month's usage and
its cost as it grows and the invoice history, one invoice, and the operator's price list editor.
Lake is priced by usage only (lake#635): rates per usage dimension, model input/output and GPU
class for GPU and GPU memory (lake#643). There are no plans, no monthly fee and nothing included,
so there are no plan cards or plan picker. Compute is CPU, RAM, GPU and GPU memory, each at its own rate. They call only the billing
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
| `/billing?customer_id=` | `billing/BillingPage.tsx` | Figures (this month so far, due, payment), **This Month So Far** (one line per rate: Usage, such as "CPU", "GPU (L4)" or "GPU, other classes", Used (with "billed as" when usage is rounded up to a whole minute, vCPU-minute, GiB-minute or GPU-minute on the month's total, lake#637, lake#643), Rate (per minute, vCPU-minute, GiB-minute or GPU-minute), Cost, where the cost's tooltip reads it as usage × rate = cost; the running total; a warning naming any workspace whose usage could not be read), **No Price List Yet** before one is published, **New Rates From <month>** when a list is published for a later month, **Invoices** (month, number, total, state), **Not Measured Yet**, **Billing Contact** ([below](#the-billing-contact)), and, while the account has none and pays online, a notice above the invoices. The account is the current workspace's; the operator passes `customer_id` and has **Price list** and **Issue an invoice** (a month that has ended). | `billing_account`, `price_list`, `bill_preview`, `list_invoices`, `my_permissions`, `set_billing_contact`, `issue_invoice` |
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

## The billing contact

lake#857: the **Billing Contact** card on `/billing`, below **Invoices**, shows where the account's
invoices go and the name and address they carry (`billing_account.contact`,
[`../api/billing.md#billing-contact`](../api/billing.md#billing-contact)).

- **Members** read it: email, **Name on invoices** ("The account's name" when it has none), the
  address as lines and **Updated** in the footer, or "No billing contact yet. An owner or admin of
  this account adds the email invoices are sent to."
- **Owners, admins and the operator** (`my_permissions` `admin` on the customer, as
  `useSpacePermissions` reads it) have **Edit**, which opens the form in the card; while no contact
  is saved the form is open, its email filled with `suggested_email` (their own; empty for the
  operator), and nothing is stored until **Save contact**. The fields are **Email** (required),
  **Name on invoices**, **Address**, **Address line 2**, **City**, **Postal code**, **State or
  region** and **Country**, a select of the 249 assigned ISO 3166-1 alpha-2 codes by English name
  ("Not set" for none). Each input carries the column's length as `maxLength`; a refusal from
  `set_billing_contact` sits under the field its path names.
- **Save contact** calls `set_billing_contact` with every field, which replaces the contact whole. The
  toast says "Billing contact saved", "… and sent to Stripe" (`provider_update` `updated`), or, when
  Stripe refused it, that it is saved and is sent again at the next save, with Stripe's message.
- **The notice.** While the account has no contact and pays online (`provider.online_payment`), a
  banner above **Invoices** says "Add a billing email so invoices are emailed to you." Invoice-only
  accounts get none: Orama sends their invoices.

The rules are in `billing.ts`: `isCountryCode`, `countryName` (`Intl.DisplayNames`),
`countryOptions` (by name; a saved code outside the list is offered too, so saving the form keeps
it), `contactDraft`, `addressLines` (street, then city, state and postal code, then the country),
`asksForBillingEmail` and `contactSavedMessage`.

Amounts are millionths of the currency in the API; the pages show them to the cent, keeping
fractions of a cent only under one unit (a price of $0.0004 a call). Quantities use Usage's units
(`formatValue`). The pure rules are `frontend/src/pages/billing/billing.ts`, tested by
`frontend/tests/billing.test.ts`.

## Words

Bills, rates and invoices say **account** and **workspace**, never `space`, and never plan. Headings are Title Case;
buttons sentence case. Prices never appear on the Usage dashboard; Billing links to Usage and back.

## Verify

`npx tsc --noEmit -p .`, `npm test` and `npm run build` in `frontend/`. With a lake started with
`LAKE_BILLING=on`, sign in as the operator, open Administration → Billing, publish the list rates,
open `/billing?customer_id=<id>` and see this month's lines and total, publish a list for next
month and see **New Rates From**, and issue last month's invoice; with
`LAKE_BILLING` unset, the rail has no Billing item and `/billing` is Not Available. For the billing
contact, open `/billing` as an account's owner with no contact: the notice shows when Stripe is the
provider, and the form is open with the owner's email; save it with an address and see the card
read it back without the notice; **Edit** reopens it filled. As a member of the same account, the
card is read-only and has no **Edit**.

Model/direction prices (LSR-095, lake#961) published through `set_price_list` are retained by
`rateRows` and `ratesInput` when the operator edits another rate. Model rows keep their exact
class keys and display casing; GPU defaults are not offered as model classes. The historical
default model-token rate covers old unclassed usage only. `frontend/tests/billing.test.ts` covers an editor round trip
with distinct input/output prices; [the gateway contract](../api/model-gateway.md#configuration)
explains the required published rates and model allowlist.
