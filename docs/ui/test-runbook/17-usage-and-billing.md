# Usage and Billing

Organization → Usage (the usage dashboard), the operator's Every Account, Organization → Billing,
one invoice, and the operator's Price List. Usage is on every Lake. Billing exists only where
`server_info.billing` is `true`; card payment through Stripe only where `online_payment` is `true`.
Each Billing case says which flags it needs. Repository Usage (`/metering`) is in
[16-administration.md](16-administration.md).

## Before you start

- Personas: `owner`, `member`, `newcomer`, `operator`.
- Test data: `W1`, `TD-W2`, `TD-ORG`, `TD-ORGWS`.
- Read the flags from `server_info` (setup.md, *Confirm the version*):
  - `billing: true`: run every case except those marked **billing off**.
  - `billing: false`: run only the cases marked **billing off**; mark the rest `blocked`.
  - `online_payment: false` (with `payment_provider` manual): the cases marked **online payment**
    are `blocked`. On 2026-10-22 every nonprod Lake has `billing` true, `online_payment` false and
    the manual provider.
- Extra data for this file alone:
  - Some usage in `W1` and `TD-W2` this month and last month (any reads, writes and a running
    workspace give some).
  - A published price list for the Billing cases (BILL-031 publishes one when none exists).
  - Two ended months for which `owner`'s account has no invoice yet (BILL-037, BILL-041).
  - With online payment: Stripe test mode and its test cards (one that pays, one that is declined).
- Limits set in this file are removed in BILL-012. Invoices cannot be deleted: list the ones this
  run issued in the results file.

## Usage

#### BILL-001 · The Usage dashboard loads · P0
As: owner · Needs: W1, TD-W2

1. Open **Organization → Usage**.

Expect:
- Eyebrow "Organization", title "Usage", sub "What your workspaces store and use over time, against
  your limits."
- Header **Export CSV** and **Set a limit**; no **Every account**.
- Toggles **All workspaces** (with a count) and one per workspace; the **Period** tabs `24 hours`,
  `7 days`, `30 days`, `This month` (pressed), `12 months`.
- One figure per dimension, a card **Over Time** with a chart, cards **Limits** and **Repository
  Usage by Hour**, and the table **By Workspace** with `W1` and `W2`.

#### BILL-002 · Usage with no workspace · P1
As: newcomer

1. Open **Organization → Usage**.

Expect:
- "Start your first workspace" with "Usage is kept per workspace, so there is nothing to show until
  one exists." and **Workspaces** (opens `/workspaces`).

#### BILL-003 · Choose workspaces · P1
As: owner · Needs: W1, TD-W2

1. Press `W1`'s toggle.
2. Press `W2`'s toggle too, then **All workspaces**.
3. Press `W1` again and reload; then open `/usage` in a new tab.

Expect:
- Step 1: `?ws=<W1 id>`; the figures and **By Workspace** cover `W1` only.
- Step 2: choosing both equals All; **All workspaces** removes `ws` from the address.
- Step 3: the choice survives the reload and is remembered in the new tab.

#### BILL-004 · Choose the period · P1
As: owner · Needs: W1

1. Press each period tab in turn.

Expect:
- `?range=24h`, `7d`, `30d`, `12m`; `This month` removes `range`.
- The chart's steps are hours for `24 hours`, days for `7 days`, `30 days` and `This month`, months
  for `12 months`.
- Each figure's sub compares with "the 24 hours before", "the 7 days before", "the 30 days before",
  "last month" or "the 12 months before".

#### BILL-005 · Figures and their change · P1
As: owner · Needs: W1

1. Read the figures on `This month`.
2. Press a figure other than Storage.

Expect:
- Each value is in its unit in words (such as `2 min`, `1.5 h`, `1 vCPU-h`, `24 GiB-h`, `12.5k
  tokens`, `2.4M calls`, `1.5 KiB`); no raw seconds or bytes.
- Each sub has an arrow and "+x%", "-x.x%", "no change" or "new this period", then "vs last month".
- Build, run and instance time read "· Detail, not billed"; Engine Storage reads "· Detail, billed
  as the engine's volume".
- Step 2: `?dimension=<name>`; that figure is ringed, the **Over Time** card shows its title and the
  chart redraws. Pressing Storage removes `dimension`.

#### BILL-006 · The chart, stacked by workspace or kind · P1
As: owner · Needs: W1, TD-W2

1. On **Over Time**, press **By kind**, then **By workspace**.
2. Press a dimension chip above the chart.
3. Hover the chart.
4. Choose `24 hours` on a dimension with no use in that time.

Expect:
- Step 1: the bands change from workspaces to kinds (such as "Repositories", "Container images");
  past six bands the rest is "Other".
- Step 2 changes `?dimension=` like a figure.
- Step 3: the legend shows each band's own value in the unit.
- Step 4: "Nothing was used in this period." instead of a chart.

#### BILL-007 · Breakdown cards · P2
As: owner · Needs: W1

1. Chart Storage, then API and MCP Calls, then Model Tokens (when used).

Expect:
- A card per breakdown the reading has: **By Kind**, **By Agent**, **By Run**, **By Model**, **By
  Workload or Model**, **By GPU Class**; none appears without rows.
- Columns **Name**, **Total** (or **Latest** for storage) and **Share** (a percentage); **Name**
  and **Total** sort.

#### BILL-008 · The By Workspace table · P2
As: owner · Needs: W1, TD-W2

1. Press a dimension's column header twice.

Expect:
- A column per dimension, in its unit; it sorts both ways.
- A workspace that could not be read shows "Not read: <message>" in red under its name, and its
  figures are not counted as zero.

#### BILL-009 · Export CSV · P1
As: owner · Needs: W1

1. Press **Export CSV**.
2. Open the file.

Expect:
- The download is `lake-usage-<from date>-<range>.csv` (`month` for This month).
- The first line is `dimension,unit,breakdown,name,period,value`; rows cover `total`, `workspace`
  (by workspace name) and `kind`, with values in raw units (bytes, seconds, calls).
- **Export CSV** is disabled while the reading loads.

#### BILL-010 · Set a limit refuses bad input · P1
As: owner · Needs: W1

1. Press **Set a limit**.
2. Choose **API and MCP Calls**, type `ten` and press **Save limit**.
3. Type `1000`, set **Warn from (percent of the limit)** to `0`, press **Save limit**; then `101`.
4. Press **Cancel**.

Expect:
- Dialog "Set a Usage Limit", subtitle "A limit warns as usage nears it. Two are enforced: above
  the storage limit new repository writes are refused, and existing content stays readable; at the
  model tokens limit agents' model calls are refused until the month ends."
- The limit field is "Limit in calls a month" for calls and "Limit in GiB" for Storage, with the
  help "Leave empty or 0 to remove the limit."; **Warn from** starts at 80.
- Step 2: "the limit must be a number of calls" under the field.
- Step 3: "warn from a whole percentage between 1 and 100" (or the browser's number check).
- Nothing is saved.

#### BILL-011 · A limit warns near its line · P1
As: owner · Needs: W1, some calls this month

1. Set **API and MCP Calls** to `1` with **Warn from** `1`; press **Save limit**.

Expect:
- Toast "Limit set".
- A warning banner "Near a Limit" with Lake's warning text.
- **Limits** card title "1 set"; a row "API and MCP Calls", "<used> of 1 calls" in red, a meter
  with a percentage, and "This month; warns from 1% · over the limit".
- Reopening **Set a limit** on calls fills `1` and `1`.

#### BILL-012 · Remove a limit · P2
As: owner · Needs: BILL-011

1. Open **Set a limit**, choose **API and MCP Calls**, clear the limit and press **Save limit**.

Expect:
- Toast "Limit removed"; the **Limits** card reads "None set" and "No limits yet. A limit warns from
  a percentage you choose; the storage limit also refuses new repository writes above it, and the
  model tokens limit refuses agents' model calls."
- The banner is gone.

#### BILL-013 · A member's limit is refused · P2
As: member · Needs: TD-ORGWS

1. Choose only `TD-ORGWS` in the toggles and press **Set a limit**.
2. Save a calls limit of `1000`.

Expect:
- **Set a limit** is shown (suspected defect 4); Lake's refusal stays in the dialog; nothing is set.

#### BILL-014 · Workspaces of two accounts · P2
As: owner · Needs: W1, TD-ORGWS

1. Choose **All workspaces** so `W1` (yours) and `TD-ORGWS` (the organization's) are both included.

Expect:
- The header has no **Set a limit**; the **Limits** card has no **Set a limit** button.
- Limit rows name their account after a "·".
- **Repository Usage by Hour** links to `/metering` with no `customer_id`.

#### BILL-015 · Not Measured Yet and Repository Usage by Hour · P2
As: owner · Needs: W1

1. Read the side column. Press **Repository Usage by Hour**.

Expect:
- When the reading names what it could not measure: a quiet banner "Not Measured Yet" with one
  sentence per item.
- The card's text "Exact repository transfer and build time, hour by hour, and the storage the
  repository limit is enforced against."; it opens `/metering?customer_id=<your account id>`.

#### BILL-016 · Usage on a phone · P2
As: owner · Needs: W1

1. At 390×844, open **Organization → Usage**.

Expect:
- Figures two to a row; the **Period** tabs scroll sideways inside their own strip.
- **Over Time**, the breakdowns and **Limits** stack in one column; the chart takes the card's
  width.
- **By Workspace** scrolls inside its box.

## Every Account (operator)

#### BILL-017 · Every Account loads · P0
As: operator

1. Open **Administration → Usage**.

Expect:
- Eyebrow "Administration", title "Every Account", sub "Each account's usage across all its
  workspaces, for capacity planning."; breadcrumb "Usage › Every Account".
- Figures **Accounts** ("<n> read") and Storage, CPU and RAM totals ("every account").
- One row per account: name, "<type> · <n> workspaces" (or "reading…", "could not be read"), a
  column per figure (`—` when missing) and **Open**; sorted by Storage, largest first.
- Past 200 accounts: the banner "The first 200 accounts are read; search the customer list for the
  rest."

#### BILL-018 · Period, search and Open · P1
As: operator

1. Press `7 days`. Type `owner`'s handle into **Search accounts…**.
2. Press **Open** on `owner`'s row.

Expect:
- Step 1: `?range=7d`; the figures re-read; the search narrows to `owner`'s account.
- Step 2: `/usage?customer_id=<id>&range=7d`.

#### BILL-019 · The operator's view of one account · P1
As: operator · Needs: BILL-018

1. On `/usage?customer_id=<owner's account id>`, read the header and toggles.
2. Press **Every account**.

Expect:
- Eyebrow "Administration"; header **Every account**, **Export CSV**, **Set a limit**.
- The toggles list `owner`'s live workspaces; the figures cover them.
- Step 2 opens `/usage/accounts`.
- A `customer_id` that does not exist shows "Couldn't load this account's workspaces" (or the 403
  or 404 title) with Lake's message.

#### BILL-020 · The operator's Usage with no account · P2
As: operator

1. Open `/usage`.

Expect:
- Record what shows. The code uses the operator's own workspaces and may show "Start your first
  workspace" with **Workspaces**, which does not fit the operator (suspected defect 5).

#### BILL-021 · An account opening Every Account · P2
As: owner

1. Open `/usage/accounts`.

Expect:
- "Not Available" with "This page is for the Lake operator." and **Home**.

## Billing (billing on)

#### BILL-022 · The Billing page loads · P0
As: owner · Needs: billing on, W1

1. Open **Organization → Billing**.

Expect:
- Eyebrow "Organization", title "Billing", sub "You pay for what you use: each kind of usage at its
  rate, with no plan and no monthly fee."; header **Usage** (opens `/usage`).
- Figures **This Month So Far** (an amount and the month, "reading usage", or "no price list yet"),
  **Due** ("Nothing" or an amount) and **Payment**.
- Cards **This Month So Far**, **Invoices** and **Billing Contact**.
- No **Price list** or **Issue an invoice** buttons.

#### BILL-023 · Payment by invoice only · P1
As: owner, then operator · Needs: billing on, online_payment false

1. As `owner`, read the **Payment** figure and the banners.
2. As `operator`, open `/billing?customer_id=<owner's account id>`.

Expect:
- **Payment** reads the manual provider's title with "card payment is not set up" (or "Orama records
  each payment" when Lake gives no reason).
- Step 1: banner "Invoice Only" with "Card payment is not set up on this Lake yet. Orama sends
  payment details with each invoice and records your payment."
- Step 2: the same banner shows Lake's own reason instead.
- No "Add a billing email so invoices are emailed to you." banner.

#### BILL-024 · This month's lines · P1
As: owner · Needs: billing on, a published price list, W1

1. Read **This Month So Far**.
2. Hover a line's **Cost**.

Expect:
- The card is titled with the month ("October 2026"). Columns **Usage** (such as "CPU", "GPU, other
  classes"), **Used** (with "billed as <x>" below when rounded up to a whole minute), **Rate** (such
  as "$0.0015 per vCPU-minute", or "not charged") and **Cost**.
- Step 2: the tooltip reads usage × rate = cost, such as "3,500 calls × $1.00 per 1,000 calls =
  $3.50".
- Footer "Total so far <amount>. The month is billed at the rates in force at its end, and invoiced
  after it ends."; the **This Month So Far** figure matches it.
- When a workspace does not answer: warning "Some Usage Could Not Be Read" with "<names>: these
  workspaces did not answer, so this bill is short until they do."

#### BILL-025 · Before a price list is published · P2
As: owner, then operator · Needs: billing on, no price list published (skip otherwise)

1. As `owner`, open **Billing**. As `operator`, open `/billing?customer_id=<owner's account id>`.

Expect:
- `owner`: banner "No Price List Yet" with "Orama has not published its rates on this Lake yet. Your
  usage is recorded, and nothing is billed until it is."; card "Nothing to bill yet" / "The bill
  appears here once a price list is published."
- `operator`: "Publish the price list so accounts can be billed for their usage." with **Publish
  it** (opens `/billing/plans`).

#### BILL-026 · Save the billing contact · P1
As: owner · Needs: billing on, no contact saved yet

1. Read the **Billing Contact** card.
2. Keep the filled **Email**; type `QA <MMDD> Ltd` in **Name on invoices**, `1 Test Street`,
   `Lisbon`, `1000-001`, choose `Portugal`; press **Save contact**.

Expect:
- Step 1: the form is open, **Email** filled with `owner`'s email; fields **Email** (required, help
  "Where invoices are sent."), **Name on invoices** (help "Your legal or company name. Left empty,
  the account's name is used."), **Address** (placeholder "Street and number"), **Address line 2**,
  **City**, **Postal code**, **State or region**, **Country** (first option "Not set", then
  countries by English name). No **Cancel**.
- Step 2: toast "Billing contact saved" (manual provider); the card reads **Email**, **Name on
  invoices** "QA <MMDD> Ltd" and **Address** as lines ending "Portugal"; footer "Updated <date>";
  an **Edit** button.

#### BILL-027 · The contact form refuses a bad email · P2
As: owner · Needs: BILL-026

1. Press **Edit**, change **Email** to `not-an-email` and press **Save contact**.
2. Clear **Email** and press **Save contact**.
3. Press **Cancel**.

Expect:
- Steps 1 and 2: the browser refuses to submit (email format, then required); or Lake's refusal
  shows under **Email**.
- Step 3: the read view returns with the saved values.

#### BILL-028 · Edit the contact and clear the name · P2
As: owner · Needs: BILL-026

1. Press **Edit**, clear **Name on invoices**, set **Country** to **Not set**, and save.

Expect:
- The form opens filled with the saved values.
- After saving, **Name on invoices** reads "The account's name" and the address has no country line.

#### BILL-029 · A member reads the contact · P1
As: member · Needs: billing on, TD-ORGWS

1. Switch the workspace pill to `TD-ORGWS` and open **Organization → Billing**.

Expect:
- The page is `TD-ORG`'s account's billing.
- **Billing Contact** has no **Edit** and no form: with no contact, "No billing contact yet. An owner
  or admin of this account adds the email invoices are sent to."; with one, the read view.

#### BILL-030 · Ask for a billing email when payment is online · P2
As: owner · Needs: billing on, **online payment**, an account with no contact

1. Open **Organization → Billing**.

Expect:
- Above **Invoices**, the banner "Add a billing email so invoices are emailed to you."
- **Payment** reads "Stripe" with "pay each invoice on Stripe's page"; no "Invoice Only" banner.
- Saving the contact gives "Billing contact saved and sent to Stripe", or, when Stripe refuses,
  "Billing contact saved, but Stripe refused it: <message>. It is sent again at the next save." in
  red.

## Price List (operator)

#### BILL-031 · The Price List · P0
As: operator · Needs: billing on

1. Open **Administration → Billing**.

Expect:
- Title "Price List", sub starting "One rate per kind of usage, and one per GPU class."; banner
  "Metis Is Not on This List" with "Metis is enterprise software priced by Orama's sales team. This
  list prices Orama Cloud usage only."
- Card label "In Force Since <Month YYYY>" (or "Not Published Yet" with "Nothing is published on
  this Lake yet. The rows start from Orama Cloud's list rates; publish them to start billing."),
  title "Rates".
- **From month** (this month), help "The list applies from the first day of this UTC month.";
  **Currency** `USD`, help "Card payment through Stripe bills USD only."
- Twenty rows: CPU, RAM, "GPU, other classes", "GPU (L4)", "GPU (A10G)", "GPU (A100)", "GPU
  (H100)", "GPU Memory, other classes" and its four classes, Storage, Transfer, Speech to Text, API
  and MCP Calls, Model Predictions, Index Searches, Model Tokens, Service and Store Volumes.
- With nothing published, CPU reads `0.0015` per `60000` with the preview "$0.0015 per
  vCPU-minute"; the GPU Memory class rows are empty ("not charged").
- **Per** labels name the unit, such as "Per (thousandths of a vCPU × seconds)".
- Side card **Publications**, newest first, the one in force marked " · in force"; or "Nothing
  published yet" / "Each publication is kept here."
- If nothing is published, press **Publish from <this month>** now: toast "Price list published
  from <Month YYYY>".

#### BILL-032 · A bad rate is refused before sending · P1
As: operator · Needs: billing on

1. Set CPU's **Rate** to `free` and press **Publish from <month>**.
2. Set it to `-1`; then set it back and set CPU's **Per** to `1.5`.
3. Press **Reset to the list rates**.

Expect:
- Steps 1 and 2: the form error "CPU: a rate is an amount, such as 0.04, and "per" a whole number
  above 0"; the preview beside the row is blank while it is invalid; nothing is published.
- Step 3: every row is back to the list rates.

#### BILL-033 · Publish rates for next month · P1
As: operator · Needs: billing on

1. Set **From month** to next month; the button reads "Publish from <next month>". Change one rate
  and press it.
2. As `owner`, open **Organization → Billing**.

Expect:
- Step 1: toast "Price list published from <next month>"; **Publications** lists it as " · next";
  the card footer adds " A list is already published for <next month>."
- Step 2: banner "New Rates From <next month>" with "The price list changes from <next month>. This
  month is billed at today's rates."

#### BILL-034 · Currency is three capitals · P2
As: operator · Needs: billing on

1. Type `eurx` into **Currency**.

Expect:
- The field holds `EUR` (uppercased, at most 3); the **Rate** labels read "Rate (EUR)" and the
  previews use the euro sign.
- Set it back to `USD` without publishing.

#### BILL-035 · An account opening the Price List · P2
As: owner · Needs: billing on

1. Open `/billing/plans`.

Expect:
- "Not Available" with "This page is for the Lake operator." and **Home**.

## Invoices

#### BILL-036 · The operator opens an account's billing · P1
As: operator · Needs: billing on

1. Open `/billing` with no query.
2. Open `/billing?customer_id=<owner's account id>`.

Expect:
- Step 1: eyebrow "Administration"; "Choose an account" with "Open an account's billing from its
  customer page, or add ?customer_id= to this address." and **Customers**. Customers has no link to
  billing (suspected defect 2).
- Step 2: `owner`'s billing with **Usage**, **Price list** and **Issue an invoice** in the header,
  and the editable **Billing Contact**.

#### BILL-037 · Issue an invoice for an ended month · P0
As: operator · Needs: billing on, a published price list, BILL-036

1. Press **Issue an invoice**.
2. Keep **Month** on last month and press **Issue invoice**.

Expect:
- Dialog "Issue an Invoice", subtitle "The bill for a month that has ended: every workspace's usage
  at the rates in force at the month's end. Its lines are kept as issued."; **Month** help "A UTC
  calendar month that has ended."
- Toast "Invoice <n> issued: <amount>".
- **Invoices** lists it: the month linked, "No. <n> · <date>", **Total** and **State** `due`
  (amber). **Due** shows the amount in red.

#### BILL-038 · Issuing this month or a month twice is refused · P2
As: operator · Needs: BILL-037

1. Issue for this month.
2. Issue for last month again.

Expect:
- Each refusal stays in the dialog with Lake's message; no invoice is added.

#### BILL-039 · The account's invoice · P1
As: owner · Needs: BILL-037

1. On **Organization → Billing**, press the invoice's month.

Expect:
- Title "Invoice <n>" with the chip `due`; sub "Usage in <Month YYYY>, at the rates in force at the
  month's end."; breadcrumb "Billing › Invoice <n>".
- Card **Invoice**: **Number**, **Month**, **Issued** and **State**.
- Card **Lines** with the same columns as the bill, footer "Total <amount> (USD)".
- With the manual provider and instructions: banner "How to Pay" with Lake's instructions; no **Pay
  with Stripe**.
- No **Void** or **Record payment** for `owner`.

#### BILL-040 · Print an invoice · P2
As: owner · Needs: BILL-039

1. Press **Print**.

Expect:
- The browser's print dialog opens with the invoice's lines and total readable.

#### BILL-041 · Record a payment · P1
As: operator · Needs: BILL-037, manual provider

1. Open the invoice from `/billing?customer_id=<owner's account id>`.
2. Press **Record payment**, type `QA-<MMDD>-REF` in **Reference** and `qa run` in **Note**; press
   **Record payment**.

Expect:
- Header **Print**, **Void** and **Record payment** (primary when there is no Stripe page).
- Dialog "Record a Payment", subtitle "Marks invoice <n> paid: <amount>."; **Reference** help "The
  payment's reference, such as a bank transfer's."
- Toast "Invoice <n> paid"; the chip is `paid` (green); **Paid** and **Reference** rows appear;
  **Void** and **Record payment** are gone.
- On Billing, **Due** reads "Nothing".

#### BILL-042 · Void an invoice · P1
As: operator · Needs: billing on, a second ended month without an invoice

1. Issue an invoice for that month and open it.
2. Press **Void**, leave **Why** empty and press **Void invoice**.
3. Type `qa test` and press **Void invoice**.

Expect:
- Dialog "Void the Invoice", subtitle "Invoice <n> is cancelled, and <Month YYYY> can be invoiced
  again."; the button is red; there is no typed confirmation.
- Step 2: the browser refuses (Why is required).
- Step 3: toast "Invoice <n> voided"; chip `void`; a **Voided** row; the note shows in a "Note"
  banner.
- That month can be issued again.

#### BILL-043 · Pay with Stripe · P1
As: owner · Needs: billing on, **online payment**, a due Stripe invoice

1. Open the invoice and press **Pay with Stripe**.
2. Pay with Stripe's paying test card and come back.

Expect:
- The button opens Stripe's hosted invoice page (an `https://` address).
- The **Stripe** row reads "Awaiting payment" before, and "Paid" after; the state becomes `paid`
  once Stripe's notice arrives (reload after a minute).
- The operator sees no **Record payment** on a Stripe invoice.

#### BILL-044 · A failed card payment · P2
As: owner · Needs: billing on, **online payment**, a due Stripe invoice

1. Pay with Stripe's declined test card, then reopen the invoice.

Expect:
- **Stripe** reads "Payment failed" (red) and the banner "The Last Payment Failed" reads "Stripe
  could not take the last payment. Pay again on Stripe's page with another card or account."
- **Pay with Stripe** is still offered.

#### BILL-045 · The Due figure's wording · P2
As: owner · Needs: BILL-037 before BILL-041

1. With exactly one due invoice, read the **Due** figure's sub.

Expect:
- Record the text. The code writes "1 invoices due" (suspected defect 1).

#### BILL-046 · An invoice link from another account's billing · P2
As: member · Needs: billing on, an invoice on `TD-ORG`'s account

1. With the workspace pill on `W1`, open `/billing?customer_id=<TD-ORG's account id>` and press an
   invoice.

Expect:
- Record whether the invoice opens. The link drops `customer_id` for anyone but the operator, so it
  is read against the current account and may fail (suspected defect 3).

#### BILL-047 · Billing on a phone · P2
As: owner · Needs: billing on

1. At 390×844, open **Organization → Billing**, then an invoice.

Expect:
- The figures stack; **Invoices** and **Billing Contact** drop below **This Month So Far**.
- The lines table scrolls inside its box; the contact form's fields are one per row.

#### BILL-050 · Give credit and revoke it from an account's Billing page · billing on · P0
As: operator, then owner · Needs: billing on, an email inbox for `owner`

1. As `operator`, open `/billing?customer_id=<owner's account id>` and press **Give credit**.
2. Enter **Amount (USD)** `25`, **Reason** `qa-<MMDD> credit`, a **Last day it counts** next month,
   and press **Give credit**.
3. As `owner`, open **Organization → Billing**, then check `owner`'s inbox.
4. As `operator`, press **Revoke** on the credit's row and confirm.

Expect:
- Step 1: the dialog "Give Credit" says the credit is spent before paid money, is not cash and does
  not unlock the overdraft. **Give credit** stays disabled until the amount and reason are valid.
- Step 2: a toast with the new balance and "the account was emailed"; the history has **Credit from
  Orama** +$25.00 with "qa-<MMDD> credit · expires <date> · $25.00 left".
- Step 3: `owner` sees the same row, no **Revoke**, and **Added** reads "Top-ups and credits from
  Orama". One email "Orama added $25.00 of credit to your Lake account", with no reason in it.
- Step 4: toast "Credit revoked: $25.00 unused stopped counting" (less what was used since); a row
  **Credit from Orama revoked** (−) appears and the credit's row has no **Revoke**.
- *API* `audit` shows `billing.balance.credit_granted` and `billing.balance.credit_revoked` on
  `customer/<id>/billing`.

#### BILL-051 · A credit alone does not unlock the overdraft · billing on · P1
As: operator, then a new account · Needs: billing on

1. Sign up a new test account `qa-<MMDD>-credit`. As `operator`, give it $1 from **Customers → Give
   credit** by its email.
2. As the new account, start a workspace, and use it until the balance reaches $0.

Expect:
- Before the credit: **Add Funds to Start**. After it: no banner; paid work starts.
- At $0: the banner **Your Credit Is Used Up**, and starting paid work answers "…credit from Orama
  is used up. Add $20.00 to go on". The **Limit** reads $0.00, never -$50.00.

## Billing off

#### BILL-048 · No Billing where the Lake does not bill · billing off · P0
As: owner, then operator · Needs: billing off

1. As `owner`, look at the Organization group, then open `/billing` and
   `/billing/invoices/any-id`.
2. As `operator`, look at the Administration group, then open `/billing/plans`.

Expect:
- No **Billing** item in either rail group, and the command palette offers no billing page.
- Every billing address shows "Not Available" with "This Lake does not bill. Usage is tracked as
  usual." and **Usage** (opens `/usage`).
- **Organization → Usage** works as in BILL-001.

#### BILL-049 · The billing message comes before the audience check · billing off · P2
As: owner · Needs: billing off

1. Open `/billing/plans` (an operator page).

Expect:
- The billing message "This Lake does not bill. Usage is tracked as usual.", not "This page is for
  the Lake operator."

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `pages/billing/BillingPage.tsx:76` | The **Due** sub is "<n> invoices due" with no singular: "1 invoices due". | BILL-045 |
| 2 | `pages/billing/BillingPage.tsx:42`; `pages/admin/CustomersPage.tsx:45` | The operator's empty state says to open billing "from its customer page", but no page links to `/billing?customer_id=`; the operator's rail Billing goes to the price list. | BILL-036 |
| 3 | `pages/billing/BillingPage.tsx:26,183`; `pages/billing/InvoicePage.tsx:24,31` | `?customer_id=` is honoured for any viewer, but the invoice link and its Billing crumb keep it only for the operator. | BILL-046 |
| 4 | `pages/usage/UsagePage.tsx:150-154,351` | **Set a limit** shows to any reader; `set_usage_limit` needs `admin` on the account. | BILL-013 |
| 5 | `pages/usage/UsagePage.tsx:68,161-168` | The operator on `/usage` without `customer_id` reads its own (empty) workspaces and gets "Start your first workspace" with **Workspaces**. | BILL-020 |
| 6 | `pages/usage/UsagePage.tsx:384-391` | **Set a limit** can open before the reading loads; **What to limit** is then empty while the dialog targets `storage`. | BILL-010 |
| 7 | `pages/billing/PriceListPage.tsx:106-110` | Every row labels its inputs only "Rate (USD)" and "Per (…)"; a screen reader hears 20 identical fields with no row name. | BILL-031 |
| 8 | `App.tsx:34`, `pages/billing/BillingPage.tsx:81` vs `docs/ui/billing.md` | The doc quotes "This lake does not bill." and "…not set up on this lake yet"; the code says "Lake" and adds "Orama sends payment details…". | BILL-048, BILL-023 |
| 9 | `docs/guide/usage.md` (intro, Set a Usage Limit) | The guide says Billing bills "on your plan" (there are no plans) and that only the storage limit refuses, contradicting the Model Tokens limit described below it and in the dialog. | BILL-010 |
| 10 | `pages/usage/UsagePage.tsx:172-174` | Mis-indented JSX around the Period tabs. Cosmetic. | BILL-004 |
