# Global checks

These hold on **every page a case opens**, without being repeated in the case. A failure of one
fails the case that was open, and the issue names the global check (`G-03`). Run the cross-app
cases at the end of this file once per run.

## On every page

| Id | Check | How to see it |
|---|---|---|
| G-01 | The tab title is `Lake — <page title>`, in Title Case (`Lake — Search Indexes`), or `Lake — <object name>` on a detail page | The browser tab |
| G-02 | No uncaught error and no React warning in the Console | Console panel, filter *Errors* |
| G-03 | No request answers 5xx, and no 4xx that the page does not explain on screen | Network panel, filter `/v1/` |
| G-04 | No raw value leaks into the page: `undefined`, `null`, `NaN`, `[object Object]`, `Invalid Date`, an unformatted ISO timestamp, a JSON blob outside a *The whole answer* disclosure | Read the page |
| G-05 | The rail marks the item the page belongs to (`aria-current="page"`), and its group is open | The rail |
| G-06 | The breadcrumb names the page's place (`Ship › Models › churn`), and each crumb but the last opens its page | The top bar |
| G-07 | The address can be shared: reloading it, or opening it in a new tab as the same persona, shows the same page, tab, filter and selection | Reload; copy the URL into a new tab |
| G-08 | Back and Forward move between the pages and tabs opened, and never leave a half-drawn page | The browser's buttons |
| G-09 | A list shows a skeleton while it loads, never an empty state that then fills in | Throttle to *Slow 4G* and reload |
| G-10 | A failed read shows an error block with Lake's message and, unless it is a 4xx, **Retry**; a 403 reads "You don't have access to this" | Block a `/v1/` request in the Network panel, or use `member` |
| G-11 | Every action that writes gives feedback: a toast (success, about 3 s), or an error that stays in its form or dialog and names the field when Lake does | Watch after each press |
| G-12 | A button that writes cannot be pressed twice: it shows a spinner or is disabled until the write answers | Double-click it |
| G-13 | Wording follows the app: Title Case headings, sentence-case buttons and messages, no internal ids in sentences except where a case says so | Read the page |

## Phone width (390×844)

| Id | Check |
|---|---|
| G-20 | No sideways scroll of the page: `document.documentElement.scrollWidth` is 390. A wide table scrolls inside its own box |
| G-21 | The top bar shows the menu button, the last crumb only, the workspace pill (name only), search, theme and the Ask Orama icon. The rail is a drawer opened by **Open the navigation** |
| G-22 | Every control has a hit area of at least 40 px, and question and dialog-footer buttons 44 px |
| G-23 | Text fields use 16 px text (no zoom on focus in iOS Safari) and are at least 40 px tall |
| G-24 | A drawer or *sheet* dialog fills the screen, and its action buttons stay above the on-screen keyboard |
| G-25 | A page's side column (facts, delete, run controls) drops below its main column |

## Keyboard and accessibility

| Id | Check |
|---|---|
| G-30 | **Tab** reaches every control in reading order, and focus is always visible. The first Tab on a page offers the skip link to the main content |
| G-31 | A dialog or drawer takes focus when it opens, keeps it inside, closes on **Escape**, and gives focus back to the control that opened it |
| G-32 | Pressing the backdrop closes a dialog only when the press starts and ends on the backdrop; a double-click on the opener does not close it |
| G-33 | Every icon-only button has an accessible name (the tooltip or `aria-label`); a sortable column header says its sort (`aria-sort`) |
| G-34 | Every error message is announced (`role="alert"`) and sits next to the field it names (`aria-invalid` on the field) |
| G-35 | With *Emulate prefers-reduced-motion: reduce*, nothing slides or animates |

## Theme

| Id | Check |
|---|---|
| G-40 | In dark theme every text, icon, border, chart and status chip is readable; nothing stays white on white or black on black |
| G-41 | Reloading keeps the theme, with no flash of the other theme |

## Cross-app cases

Run these once per run.

#### G-101 · Every rail item opens its page · P0
As: owner, then member, then operator, then visitor

1. Press each rail item in turn, top to bottom.

Expect:
- Each opens with G-01 to G-06 holding.
- `owner` and `member` see the Home group (**Ask Orama**, **Dashboard**), Data, Design, Train, Ship,
  Network, Organization and Developer. Billing shows only when `server_info.billing` is `true`.
- `operator` sees **Home**, Data, Design, Train, Ship, Network, Administration and Developer, and no
  Organization group.
- `visitor` sees **Home**, **Public Repositories** and **Docs** only.

#### G-102 · A visitor is sent to sign in, and back · P0
As: visitor

1. Open `https://<host>/w/<W1 id>/ship/models?view=x` directly.
2. Sign in as `owner`.

Expect:
- Step 1 lands on `/sign-in?next=` followed by the encoded path and query.
- After sign-in the browser is on the page that was asked for, with its query.

#### G-103 · Pages for another audience say so · P1
As: operator, then owner

1. As `operator`, open `/dashboard`, `/settings` and `/tokens`.
2. As `owner`, open `/audit` and `/usage/accounts`.

Expect:
- Step 1: a page titled "Not Available" saying "This page belongs to an account; the operator
  credential has no account." with **Home**.
- Step 2: "Not Available" with "This page is for the Lake operator." and **Home**.

#### G-104 · An ended session is caught on the page · P0
As: owner, in two browsers

1. In browser A, open **Data → Datasets** and type into the search box.
2. In browser B, open **Account → Sessions** and revoke browser A's session.
3. In browser A, press any button that reads or writes.

Expect:
- Browser A shows "Your session has ended" / "Sign in again to carry on. You'll come back to this
  page." with **Sign in again**; the typed text is still in the box.
- **Sign in again** returns to the same address after signing in.

#### G-105 · A permission refusal is shown, not hidden · P1
As: member

1. On `W1`, try one write in each group: load `TD-CSV` with **Load data** (Data → Datasets), create
   a project with **New project** (Design → Projects), and create a service with **New service**
   (Ship → Services).

Expect:
- Each refusal is shown where the action was taken, with Lake's message (`unauthorized` or "You
  don't have access to this"). No button spins forever, and nothing is created.
- Where a section file says a control is hidden from a reader (Streams, Train → Runs), it is hidden.

#### G-106 · A lost connection is explained · P1
As: owner

1. Open **Ship → Services**, then set the Network panel to *Offline*.
2. Press **Retry** on the error that appears, or reload a list.
3. Set it back to *No throttling* and press **Retry**.

Expect:
- Step 2: "Lake could not be reached. Check your connection and retry."
- Step 3: the list loads; no stale error stays.

#### G-107 · An unknown address shows Not Found · P1
As: owner, then visitor

1. Open `/no-such-page` and `/w/<W1 id>/no-such-page`.

Expect:
- Inside the shell: "We Could Not Find This Page" and "Nothing is at `/no-such-page`. Check the
  address, or start again from Home." with **Home**.

#### G-108 · A deleted workspace's address moves to a live one · P2
As: owner

1. Open a page under `/w/<id of a workspace deleted in this run>/train/data`.

Expect:
- The address is replaced by the same page under a live workspace, with the query kept.

#### G-109 · A new build is picked up · P2
As: owner

1. Keep a page open across a roll of a new lake image (only when a roll happens during the run).
2. Open another page from the rail.

Expect:
- The page opens. At most one automatic reload happens; no blank page and no "This page hit an
  error".
