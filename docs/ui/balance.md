# Balance pages

lake.live is prepaid ([`../api/billing.md#balance`](../api/billing.md#balance), lake#1114):
each account has a balance, credits from top-ups minus charges for its
usage. The web app nags while it is low, negative or blocked, and gives one place to add funds.
Every surface here exists only where the lake bills (`viewer.billing`); none shows for the operator,
or for an account a paid cloud order covers.

## The banner on every page

`frontend/src/components/BalanceBanner.tsx`, rendered by the shell above every page. It reads
`viewer.balance`, the current account's balance, which `viewer` answers on every page load (and
again when the session refreshes). `bannerFor` (`frontend/src/pages/billing/balance.ts`) decides:

| State | Title | Says |
|---|---|---|
| `ok` | — | nothing is shown |
| `low` | Your Balance Is Low | the balance and how much to add |
| `negative` | Your Balance Is Below $0 | the balance, the limit where paid work stops, how much to add |
| `blocked` (overdraft) | Paid Work Is Stopped | the balance at its limit, that running work keeps running and reading, stopping and deleting still work |
| `blocked` (first top-up) | Add Funds to Start | the first top-up, and that Lake is prepaid |

Each has **Add funds**, which opens `/billing/add-funds?customer_id=<account>`. The banner is not
shown on that page itself.

## Add funds

`/billing/add-funds?customer_id=` (`frontend/src/pages/billing/AddFundsPage.tsx`): the one page
every balance email, the banner and every `402 payment_required` answer link to. A visitor is sent
to sign in and back (the router's `next`). It shows **Balance** (with its state), **Limit** and
**Suggested** (`add_micros`), and **Add Funds**: an amount in US dollars, prefilled with the first
top-up (read-only: the first top-up is exactly that amount) or the suggestion, checked by
`topUpRule` (whole cents, at least the minimum, at most $10,000). **Add $N** calls `start_top_up`
and goes to Stripe's Checkout page; Lake never sees the card.

Stripe sends the person back with `top_up=done` ("Payment received. Your balance updates as soon as
Stripe confirms it") or `top_up=cancelled` ("Nothing was charged"). After `done` the page reads
`account_balance` every three seconds until a new credit appears, says **Funds Added**, and
refreshes the session so the banner goes.

## The Billing page

`/billing` (`BillingPage.tsx`) gains, above the bill:

- **Balance**: the balance with its state ("covered by your order" for a paid order), **Added**
  (top-ups) and **Limit**, **Add funds** for owners and admins, and the history: each top-up and
  auto top-up (+) and each month's usage charge (−), newest first (`historyRows`). The footer says
  when charges were last refreshed, or, when a refresh could not read every workspace, that the
  balance is kept as it was until it can.
- **Auto Top-Up** (in the side column): off by default. Owners and admins set **When the balance
  falls below** (default $5) and **Add** (default $20, never under the minimum) and press **Save a
  card and turn on**, which goes to Stripe to save a card (`set_auto_top_up` answers its
  `checkout_url`), then **Turn on** / **Save** / **Turn off**. After a declined charge it shows
  **Turned Off** with the reason. Members read it.
- Back from Stripe's card page, `auto_top_up=saved` or `cancelled` is said in a banner.

- **Credits from Orama** (lake#1134) are in the history as **Credit from Orama** (+), with the
  reason, the last day it counts and what is left (`grantDetail`); one that expired or was revoked
  adds a row **Credit from Orama expired** / **revoked** (−) for the unused part that stopped
  counting. **Added** says "Top-ups and credits from Orama" and how much credit is left. Once a
  grant-only account's credit is used up, the banner reads **Your Credit Is Used Up**.

Capabilities: `viewer`, `account_balance`, `start_top_up`, `set_auto_top_up`.

## Give credit

The operator's (lake#1134, [billing.md](../api/billing.md#credits-from-orama)). `GiveCredit.tsx`
is one dialog opened from two places:

- **Customers → Give credit** (`CustomersPage.tsx`, where the Lake bills): **Account ID or email**
  (a person's email gives the credit to their personal account), **Amount (USD)** (up to $10,000),
  **Reason** (the account sees it) and **Last day it counts (UTC)** (optional; the credit's unused
  part stops counting at the end of that day).
- **Billing → Give credit** on an account's Billing page (the account is the one open).

`grantInput` in `balance.ts` turns the form into `grant_balance_credit`'s input and says what is
missing. The toast says the new balance and whether the account was emailed. In the Billing page's
history, a credit from Orama that still counts has **Revoke** for the operator: a confirmation, then
`revoke_balance_credit`, and a toast with the unused amount that stopped counting. Lake has no staff
area beyond the operator's pages, and the server refuses both capabilities to anyone but the
operator or the instance administrator.

## Coupon codes

lake#1156 ([billing.md](../api/billing.md#coupon-codes)). Three places in the console:

- **Administration → Coupons** (`/coupons`, `pages/admin/CouponsPage.tsx`; the operator, where the
  Lake bills). The codes, newest first: **Code**, **State** (Active, Disabled, Expired, Used up),
  **Credit** (hover: how many days it lasts), **Campaign**, **For** (New accounts / Any account),
  **Redemptions** ("12 of 500" with a cap), **Went On to Pay** (a first paid top-up after redeeming),
  **Spend**, **Code Expires**, and **Disable** (a confirmation; credit already given stays). Under
  it, **Campaigns** sums each campaign: codes, redeemed, went on to pay, spend and paid in.
  **New code** opens a dialog: **Code** (empty: generated), **Credit (USD)**, **Credit lasts
  (days)** (default 90), **Last day the code works (UTC)**, **Most redemptions**, **Campaign** and
  **New accounts only** (checked). `couponInput` in `lib/coupon.ts` turns it into `create_coupon`'s
  input. Once created, a banner shows **Link for the ad** (`https://lake.live/?code=LAKE50`, with a
  copy button) and, with other regions, says to create the same code there too: each region is a
  separate Lake (lake#1159 is creating it in every region at once).
- **Billing → Redeem a Code** (`pages/billing/RedeemCode.tsx`, in the side column, for an owner or
  admin of the account, not the operator): a **Coupon code** field and **Redeem**, which calls
  `redeem_coupon` for the account open. A toast says "You have $50.00 of credit from Orama."; the
  balance's history then lists the **Credit from Orama** row (reason "Coupon code LAKE50"). A refusal
  is shown under the form in Lake's words ("This code has expired.").
- **A `?code=` link, through sign-up.** `main.tsx` reads `?code=` before the app starts
  (`captureCouponCode`), keeps the normalized code in this browser's `localStorage` (`lake-coupon`,
  with the time) and takes `code` out of the address (`history.replaceState`), so a reload or a
  shared link does not carry it. localStorage, not a server session, because the sign-up leaves the
  page: Clerk's hosted pages and OAuth providers, an email verification link opened in another tab,
  and the `/sign-in?next=` redirect, which does not keep the query. The code is kept for a day. On
  the first signed-in page, `CouponWelcome` (in the shell, above the balance banner) redeems it once
  for the person's own account (`personalAccount`) and shows **Welcome to Lake** with "You have
  $50.00 of credit from Orama.", or **Code LAKE50 Was Not Applied** with Lake's reason. Lake's answer
  (a credit or a refusal) forgets the code; a network failure keeps it for the next page, within the
  day (`redemptionIsSettled`). Nothing happens where the Lake does not bill or for the operator.

The apex picker on `lake.live` carries `?code=` to the region the person chooses
([Regions](regions.md#coupon-codes)).

Capabilities: `create_coupon`, `list_coupons`, `disable_coupon`, `redeem_coupon`.

## Verify

- `frontend/tests/balance.test.ts`: the banner per state (none while ok or covered, "used up" after
  a credit from Orama), the Add funds link, dollar amounts in whole cents, the first-top-up rule,
  the history order (credits from Orama, their expiry rows), the return notices, and the Give credit
  form's input (`grantInput`).
- `frontend/tests/coupons.test.ts`: `?code=` kept through sign-up (normalized, out of the address,
  a day, replaced by a later link, forgotten once Lake answers and kept over a network failure), the
  person's own account, and the New code form's input (`couponInput`).
- `npm run build` (types, the tests, the effect and empty-state checks) and
  `./tools/ui/test-token-gates.sh`.
- In a browser on a billing Lake: an account with no top-up sees **Add Funds to Start** on every
  page; Add funds goes to Stripe; after paying, the balance shows the credit and the banner goes.

## Change

- Banner wording: `bannerFor` in `balance.ts` and its tests; keep the titles in Title Case.
- The amounts and limits come from the server (`account_balance`, `viewer.balance`): change them
  with the `LAKE_BALANCE_*` variables ([configuration](../serving/configuration.md#prepaid-balance)),
  never in the web app.
