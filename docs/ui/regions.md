# Regions in the Web App

lake#757, decision 64. The API, the variables and the apex picker are in
[`../api/regions.md`](../api/regions.md).

## What exists

Where Lake runs in regions (production: `eu.lake.live`, `us.lake.live`), the web app shows which
region it is in. Each region is a separate Lake with its own accounts, so another region is always
a link to that region's host, never a switch inside this Lake. Everything is read from
`server_info`'s `region` and `regions`; a Lake without a region (nonprod, local, an enterprise
install) answers `null` and `[]`, and the app shows none of what follows.

| Where | What it shows | Code |
|---|---|---|
| The top bar, every page in the shell | The region's label with a globe icon (the label hides below `sm`). With other regions, a button opening a menu: one sentence ("You are in Europe. Each region is a separate Lake with its own accounts.") and a link per other region, `https://<host>/`, with its host | `RegionMenu`, `frontend/src/components/Region.tsx`, in `Shell.tsx` |
| Sign in (`/sign-in`, lake's own and Clerk's) | "You are signing in to Lake in Europe. An account belongs to the region it was created in.", then links to the other regions | `AccountRegionNote mode="sign-in"` in `SignInPage.tsx`, `ClerkSignIn.tsx` |
| Create an account (`/sign-up`, both) | "Your account is created in Europe. Each region is a separate Lake with its own accounts, and your workspaces stay in Europe.", then links to the other regions | `AccountRegionNote mode="sign-up"` in `SignUpPage.tsx`, `ClerkSignIn.tsx` |
| The top bar's region menu, where `server_info.order_sheet_url` is set (lake#831) | After the other regions, **New setup…** (the order sheet) and **On-prem…** (its on-prem order form); the menu opens even with no other region | `RegionMenu`, `orderSheetLinks` in `frontend/src/lib/regions.ts` |
| Ship → Workspaces | The page's sub-line ends "Workspaces here are in Europe."; each workspace's second line shows the region after its environment; **New workspace** says "Lake prepares it in Europe, where it stays"; beside **New workspace**, **New setup…** and **On-prem…** where there is an order sheet | `WorkspacesPage.tsx`, `OrderSheetLinks` |

The words come from the pure functions in `frontend/src/lib/regions.ts` (`currentRegion`,
`otherRegions`, `regionUrl`, `accountRegionSentence`, `workspaceRegionLabel`). `server_info` is read
once with `useRead("server_info", undefined, {workspace: null})` and kept for the page's life
(`staleTime: Infinity`): it changes only when the Lake restarts.

Why links to the host's front page and not the same path: the other region has other accounts and
other workspaces, so the current path (a workspace id, a repository) means nothing there.

## The order sheet

lake#831. The apex host is the order sheet, server-rendered by `web_ui_order_sheet`
(`src/api/web_ui.cpp`) in the hosted pages' design tokens, working without JavaScript. The contract
(routes, configuration, paths, email, audit) is [`../api/orders.md`](../api/orders.md).

- **Header**: "Get Lake", that prices are in US dollars (USD), and the trust line: each claim from
  the configuration (SOC 2, source available) as a link to what backs it.
- **Four cards**, side by side at 1440 px (`grid-template-columns: repeat(auto-fit, minmax(16rem,
  1fr))`), one column on a phone; nothing has a fixed width wider than 16rem:
  - **Shared**: "Usage-based, USD, by the minute", a link to the published price list
    (`/order/price-list`), its features (backups included), and the region picker's choice and
    **Continue** button unchanged. The lead line "Train and serve a model in 5 minutes for under $5"
    appears above the price only when switched on ([billing.md](../api/billing.md#the-shared-tiers-promise)).
  - **Private Warehouse**, **Dedicated Cluster**, **On-Prem**: the monthly price in USD ("Starting
    from" for on-prem), the included usage credit and that usage beyond it is billed at the price
    list, the summary, the providers and regions (dedicated cluster), the features (each with
    backups included), and **Order** (or **Send an order form** for on-prem) to the offer's form.
- **Contact us** under the cards opens the dedicated cluster's form with "Another provider, hardware
  or region" chosen.
- **The order form** (`/order/<offer>`, `web_ui_order_form`): where it runs (a select of the shared
  regions, or of every provider's regions grouped by provider, a provider the operator does not
  provision marked "(order form)", and "not listed"), name, email, company, quantity, free text, the
  hidden honeypot field, and the two buttons: **Pay $X USD / month with card** (where card payment
  is available and the offer is paid) and **Send as an order form**. Errors show beside their field
  with `aria-invalid`.
- **Order Received** and **the status page** (`/orders/<id>?token=…`): the order id, the state in
  words, what was ordered, and the host as a button once ready.

Checked by screenshot at 1440 px and 500 px (headless Chrome will not lay out narrower than 500 px);
the layout is fluid below that.

## How to verify

- `cd frontend && npm test` runs `tests/regions.test.ts`: the current and other regions, a Lake
  without regions, the host links, the sign-in, sign-up and workspace wording, and the order sheet
  links (`orderSheetLinks`: only an https URL, always the sheet's origin).
- `ctest --test-dir build -R OrdersApex` renders the order sheet and its forms over HTTP.
- In a browser, start a local Lake with the variables in
  [`../api/regions.md`](../api/regions.md#how-to-verify) and open `http://127.0.0.1:4900/sign-in`:
  the region sentence is under the title; signed in, the top bar shows **Europe** and its menu
  links to `https://us.lake.live/`. Without the variables none of it appears.

## How to change it

- The wording is in `frontend/src/lib/regions.ts`; change its test with it.
- A new place that should show the region reads `useServerRegions()` from
  `frontend/src/components/Region.tsx`.
