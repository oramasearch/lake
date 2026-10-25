# Account Settings

A signed-in person's own pages, from the account menu at the foot of the rail: **Settings**
(`/settings?tab=`: Profile, Identity, Sessions and devices, Passkeys, Tokens, Notifications, and
Delete account), **Sessions** (`/sessions`), **Passkeys** (`/passkeys`) and **API Tokens**
(`/tokens`). The operator has none of them ([G-103](global-checks.md#g-103--pages-for-another-audience-say-so--p1)).
Sign-in, MFA and passkey sign-in are in [02-sign-in.md](02-sign-in.md).

## Before you start

- Personas: `owner` (Clerk Lake), `member`, and the `local` accounts below.
- Data: `TD-ORG` with `member` in it (for the blocked deletion), `TD-LOCAL` from
  [AUTH-011](02-sign-in.md) with the passkey from AUTH-024.
- This file makes:
  - `TD-DELETE`: on `local`, sign up `qa-<MMDD>-delete@<test inbox domain>` at `/sign-up` at the
    start of this file, in a private window, and leave it signed in. ACCT-026 needs it signed in for
    more than 15 minutes; ACCT-027 deletes it.
  - Tokens `qa-<MMDD>-tok2` (ACCT-019), `qa-<MMDD>-tok` (ACCT-020) and maybe `qa-<MMDD>-tok3`
    (ACCT-023); all are revoked by ACCT-023.
- A second browser or profile, browser B, for ACCT-009 and ACCT-012.
- `liquid` on the tester's machine for ACCT-005 (it permanently changes `TD-LOCAL`'s key).
- Passkeys and MFA are Lake's own sign-in: their cases run on `local`.

## Settings

#### ACCT-001 · Open Settings · P0
As: owner

1. Open the account menu and press **Settings**.

Expect:
- `/settings`, breadcrumb Account › Profile, tab title "Lake — Settings".
- Eyebrow "Account", title "Settings", sub "Your profile, how you sign in, and the credentials you
  hold."
- Tabs "Profile" (current), "Identity", "Sessions and devices", "Passkeys", "Tokens",
  "Notifications".
- Card "Profile" titled with the display name, rows "Identity" (the TAAP id as a link), "Handle"
  and "Email".
- Card "Multi-factor authentication" with a chip ("Enabled" or "Not enrolled"), "A TOTP
  authenticator for password sign-in." and **Manage MFA** (→ `/mfa`).
- Card "Delete account" / "Delete your account" with "Deletes your workspaces and the
  organizations you alone own, signs you out everywhere and erases your email and name. It cannot
  be undone. Sign in again first if you signed in more than 15 minutes ago." and the danger
  **Delete account**.

#### ACCT-002 · Tabs follow the address · P1
As: owner

1. Press each tab in turn.
2. Open `/settings?tab=nosuch`.

Expect:
- Step 1: the address becomes `/settings?tab=<key>`; the breadcrumb's last crumb is the tab's name;
  the pressed tab is current.
- Step 2: the Notifications content ("Notifications" / "No notification delivery preferences are
  configured.") with "Notifications" marked current; the breadcrumb ends "Notifications".

#### ACCT-003 · The other tabs · P1
As: owner

1. Open **Sessions and devices**, **Passkeys** and **Tokens**, pressing each tab's button.

Expect:
- Sessions and devices: stats "Sessions" and "Devices", each "<n> active" (both link to
  `/sessions`), and **Manage sessions and devices** (→ `/sessions`).
- Passkeys: card "Passkeys" / "Sign in with a passkey", "Registered passkeys appear as devices on
  Sessions." and **Manage passkeys** (→ `/passkeys`). It also shows on a Clerk Lake (see Suspected
  defects 1).
- Tokens: card "Tokens" / "API tokens", "Named, scoped tokens for the API, MCP and Liquid push and
  pull." and **Create or revoke tokens** (→ `/tokens`).

#### ACCT-004 · Identity with a managed key · P1
As: owner

1. Press the Identity link on Profile (or the **Identity** tab).

Expect:
- Card "Identity" titled "Managed Key" with the chip "Managed key", "Managed key: Lake holds a
  temporary key for you.", rows "Account identity" and "Signs as", and "Your grants, activity and
  repositories belong to the account identity, so they stay the same whichever key it signs with."
- Card "Optional" / "Use My Own Identity" with "Replace the managed key with your own liquid
  identity, … This cannot be undone.", "1. Run `liquid identity show --document` and paste its
  output.", **Your identity document** and **Continue** (disabled while the box is empty).

#### ACCT-005 · Use my own identity · P2
As: TD-LOCAL · Needs: `local`, `liquid`

1. On **Identity**, paste `not a document` and press **Continue**.
2. Paste the output of `liquid identity show --document` and press **Continue**.
3. Type `hello` in **What liquid sign printed**.
4. Press **Start again**, then **Continue** again with the document.
5. Run the shown command, paste everything it prints, and press **Use my own identity**.

Expect:
- Step 1: a red notice with Lake's refusal; the form stays on step 1.
- Step 2: "2. Sign this challenge with your identity before <time>, and paste everything the command
  prints.", a copy block "Run" with `liquid sign artifact <challenge>`, and **What liquid sign
  printed**.
- Step 3: **Use my own identity** stays disabled.
- Step 4: **Start again** returns to step 1 with the document kept.
- Step 5: no dialog asks first (see Suspected defects 2). The card becomes "Your Own Key" with the
  chip "Own key", "Your own key: the managed key was replaced and destroyed.", rows "Linked" and
  "Managed key destroyed"; the Optional card is gone.
- A refused signature shows a red notice and returns to step 1.

#### ACCT-006 · Identity cannot be read · P2
As: owner

1. Block `account_identity` in the Network panel and open `/settings?tab=identity`.

Expect:
- "Couldn't load your identity" with Lake's or the network's message and **Retry**.

## Sessions and devices

#### ACCT-007 · Open Sessions · P1
As: owner

1. Open the account menu and press **Sessions**.

Expect:
- Breadcrumb Account › Sessions. Eyebrow "Account", title "Sessions and Devices", sub "Revocation
  takes effect on the next request."
- Card "This session" titled with its name (or kind), the badge "Active", rows "Kind", "Started"
  (relative, full date on hover) and "Expires", and the danger **Revoke and sign out** and
  **Sign out everywhere** (lake#1163; it ends every session, key and device of the account).
- Section "Sessions" with "<active> active of <total>.", a search "Search sessions…", kind chips
  "Any kind" plus one per kind with a count ("Session", "API token", …), and state chips "Any
  state", "Active", "Revoked".
- A table of the other sessions: "Session" (name and kind), "Scopes" (up to four chips and "+<n>",
  or "—"), "Expires", "Status", and **Revoke**.
- Section "Devices".

#### ACCT-008 · Search and filter sessions · P2
As: owner

1. Type part of a token's name in "Search sessions…".
2. Clear it; press the "API token" chip; then "Revoked"; then "Any state" and "Any kind".
3. Type `zzqqxx`.

Expect:
- Steps 1 and 2: only matching rows show.
- Step 3: "No other sessions" / "Clear the search or the filters to see more."
- When this browser's session is the only one: "No other sessions" / "This browser's session is the
  only one."

#### ACCT-009 · Revoke another session · P1
As: owner, in two browsers

1. In browser B, sign in as `owner`.
2. In browser A, open **Sessions**, find B's session, and press **Revoke**.
3. In browser B, press anything that reads.

Expect:
- Step 2: no dialog asks first (see Suspected defects 3); the button spins; the toast "Session
  revoked"; the row's Status becomes "Revoked" and its **Revoke** is disabled.
- Step 3: browser B shows "Your session has ended".

#### ACCT-010 · Revoke a device · P2
As: TD-LOCAL · Needs: `local`, AUTH-024's passkey

1. In "Devices", read the cards.
2. Press **Revoke** on the passkey's device.
3. Sign out and try **Use a passkey** with it.

Expect:
- Step 1: a card per device: "Device", a laptop icon and its name, the badge, "Added <relative>".
- Step 2: no dialog; the toast "Device revoked"; the card is greyed with "Revoked" and **Revoke**
  disabled.
- Step 3: the passkey no longer signs in.
- A person with no device sees "No devices" / "A device appears here when you sign in with MFA or
  register a passkey."

#### ACCT-011 · Sessions cannot be read · P2
As: owner

1. Block `list_devices` in the Network panel and reload `/sessions`.

Expect:
- The Devices section shows "Couldn't load devices" with **Retry**; the Sessions section still
  loads.

#### ACCT-012 · Revoke this session · P1
As: owner (browser B, signed in again after ACCT-009)

1. On **Sessions**, press **Revoke and sign out**.
2. Press **Cancel**.
3. Press it again, then **Revoke**.

Expect:
- Step 1: a dialog "Revoke this session?" with "This browser is using it. You will be signed out on
  your next request." and **Cancel** / **Revoke** (danger).
- Step 2: nothing changes.
- Step 3: the browser goes to `/sign-in`; reopening `/sessions` sends to `/sign-in?next=…`.

## Passkeys

#### ACCT-013 · The Passkeys page · P1
As: TD-LOCAL · Needs: `local`

1. Open the account menu and press **Passkeys**.

Expect:
- Breadcrumb Account › Passkeys. Eyebrow "Account", title "Passkeys", sub "Register an
  attestation-less passkey for this account."
- Card "New passkey" / "Register a passkey" with **Passkey name** (prefilled "passkey"), the help
  "Name it after the device or key, so you can tell it apart later." and **Register passkey**.
- Side card "Where they appear": "Registered passkeys appear as devices on Sessions, where you can
  revoke them." and **Sessions and devices** (→ `/sessions`).

#### ACCT-014 · Register a passkey, and cancel one · P1
As: TD-LOCAL · Needs: `local`, a passkey-capable browser

1. Set **Passkey name** to `qa-<MMDD>-key2`, press **Register passkey**, and cancel the browser's
   prompt.
2. Press **Register passkey** again and complete the prompt.
3. Open **Sessions**.

Expect:
- Step 1: a yellow banner with the browser's message; the button is enabled again.
- Step 2: a green banner "Passkey registered."
- Step 3: "Devices" lists a device named `qa-<MMDD>-key2`.

#### ACCT-015 · Passkeys under Clerk · P2
As: owner · Needs: a Clerk Lake

1. Open the account menu.
2. Open `/passkeys` directly.

Expect:
- Step 1: no "Passkeys" item.
- Step 2: the page still opens; registering is not part of Clerk's sign-in (record what Lake
  answers).

## API Tokens

#### ACCT-016 · Open API Tokens · P1
As: owner

1. Open the account menu and press **Tokens**.

Expect:
- Breadcrumb Account › Tokens. Eyebrow "Account", title "API Tokens", sub "Create named, scoped
  sessions and revoke them here." and the primary **New token**.
- With tokens: stats "Tokens", "Active" and "Never expire" ("no expiry"); a search "Search
  tokens…" and chips "Any state", "Active <n>", "Revoked <n>"; the table "Token" (name and kind),
  "Scopes", "Expires", "Status", **Revoke**.
- With none: no stats, and "No API tokens" / "A token lets a script, an agent or Liquid act with
  your permissions, limited to the scopes you name." with **New token**.

#### ACCT-017 · The New Token drawer · P1
As: owner

1. Press **New token**.
2. Press **Use these** in the scopes help.
3. Press **Cancel**.

Expect:
- Step 1: a drawer "New Token", "Its secret is shown once, after it is created.", fields **Name** *
  (focused), **Capability scopes, comma separated** * with the help "For Liquid push and pull, use
  `refs,find_missing,push,pull,upload_chunks,read_chunk` Use these. The token keeps your existing
  repository permissions.", **Expiry in seconds** (help "Leave empty for as long as this session
  allows, at most 30 days."), the checkboxes "Static exception" and "Never expires (survives
  sign-out; needs your workspace's opt-in, a reason and named scopes)", **Reason for a static or
  never-expiring token**, and **Cancel** / **Create token**.
- Step 2: the scopes box holds `refs,find_missing,push,pull,upload_chunks,read_chunk`.
- Step 3: the drawer closes and nothing is made.

#### ACCT-018 · New token refuses bad input · P1
As: owner

1. Open **New token** and press **Create token** with everything empty.
2. Type **Name** `qa-<MMDD>-bad`, **Capability scopes** `no_such_capability`, press **Create
   token**.
3. Set the scopes to `list_repositories` and **Expiry in seconds** to `0`; press **Create token**.

Expect:
- Step 1: the browser asks for **Name** and the scopes; nothing is sent.
- Step 2: Lake refuses the unknown scope; the message sits under **Capability scopes, comma
  separated** (or in a red notice when Lake names no field). The drawer stays open.
- Step 3: the browser refuses `0` (minimum 1).

#### ACCT-019 · Spaces and commas in scopes · P2
As: owner

1. Type ` list_repositories, ,get_repository ` in the scopes, name it `qa-<MMDD>-tok2`, and create
   it.
2. Press **Done**.

Expect:
- The token is made with exactly `list_repositories` and `get_repository` (spaces trimmed, the
  empty part dropped); its row shows those two chips.

#### ACCT-020 · Create a token · P0
As: owner

1. Open **New token**, type **Name** `qa-<MMDD>-tok`, press **Use these**, set **Expiry in seconds**
   to `3600`, and press **Create token**.
2. Copy the token, then press **Done**.

Expect:
- Step 1: the drawer closes; the page is at the top with a green banner "Copy this token now. It is
  not shown again." a copy block "Token" and **Done**. The table lists `qa-<MMDD>-tok`: "API
  token", the six scope chips, Expires in about an hour (relative, full date on hover), Status "Active".
- Step 2: the banner goes; reloading never shows the secret again.

#### ACCT-021 · Search and filter tokens · P2
As: owner

1. Type `tok2` in "Search tokens…".
2. Clear it, press "Revoked", then "Active".
3. Type `zzqqxx`.

Expect:
- Steps 1 and 2: only matching rows.
- Step 3: "No API tokens" / "Clear the search or the filter to see more." with no **New token**.

#### ACCT-022 · Revoke a token · P1
As: owner · Needs: ACCT-020

1. Press **Revoke** on `qa-<MMDD>-tok`.

Expect:
- No dialog asks first (see Suspected defects 3); the toast "Token revoked"; Status "Revoked";
  **Revoke** disabled; the "Active" count drops by one.
- Developer → API's Your Keys no longer lists it (unless **Show revoked and expired** is ticked).

#### ACCT-023 · A never-expiring token · P2
As: owner

1. Open **New token**, name it `qa-<MMDD>-tok3`, scopes `list_repositories`, tick **Never
   expires**, leave **Reason …** empty, and create it.
2. Fill **Reason for a static or never-expiring token** and create it again.
3. Revoke `qa-<MMDD>-tok2` and, if made, `qa-<MMDD>-tok3` (`qa-<MMDD>-tok` was revoked in ACCT-022).

Expect:
- Step 1: Lake refuses with its message under the reason or in a red notice.
- Step 2: when the workspace has not opted in, Lake refuses again saying so; when it has, the row
  reads "Never expires" with the reason under it and the "Never expire" stat counts it.
- Step 3: both read "Revoked".

## Delete account

#### ACCT-024 · Deleting is blocked while you alone own an organization with members `v0.4.0+` · P1
As: owner · Needs: TD-ORG with `member` in it

1. On **Settings → Profile**, press **Delete account**.
2. Read the dialog and press the organization's link.
3. Go back, open it again and press **Cancel**.

Expect:
- Step 1: a dialog "Delete your account" with "Deleting your account removes everything below. It
  cannot be undone." and a list: "· Your account. Your email is released, and your handle and
  identity are retired.", "<n> workspaces, with everything in them", "<n> sessions and tokens,
  signed out at once", …, "· Invoices and the audit log are kept, as the records of what happened."
  (lines with a zero count are left out).
- A yellow box "Give this organization another owner first" / "You are the only owner, and other
  people belong to it. Make another member an owner, or delete the organization from its page."
  and a link "qa-<MMDD>-org (1 other member)". No typed field; **Delete account** is disabled.
- Step 2: `/organization?id=<TD-ORG id>` opens.
- Step 3: nothing is deleted.

#### ACCT-025 · A member who owns nothing is not blocked `v0.4.0+` · P2
As: member

1. Press **Delete account** and read the dialog.
2. Press **Cancel**.

Expect:
- The list includes "1 organization you leave to their other owners"; no yellow box; the field
  "Type `<member's handle>` to confirm" with **Delete account** disabled.
- Nothing is deleted.

#### ACCT-026 · An old sign-in is refused `v0.4.0+` · P2
As: TD-DELETE · Needs: `local`, signed in more than 15 minutes ago

1. On **Settings → Profile**, read **Handle**, press **Delete account**, type the handle and press
   **Delete account** in the dialog.

Expect:
- Lake refuses; its message (asking to sign in again) stays in red in the dialog; the account
  still works.

#### ACCT-027 · Delete an account: wrong text, then the right one `v0.4.0+` · P0
As: TD-DELETE · Needs: `local`

1. Sign out and sign in again as TD-DELETE.
2. On **Settings → Profile**, read **Handle**. Press **Delete account**.
3. Type the handle in capitals and read the button; then type `delete`.
4. Type the handle exactly and press **Delete account** in the dialog.
5. Try to sign in again as TD-DELETE.

Expect:
- Step 2: a skeleton, then the list "· Your account. …", "1 session or token, signed out at once"
  (or more), a line counting credentials and devices ("… (passwords, passkeys, MFA)"), "· Invoices and the audit
  log are kept, as the records of what happened." and "Type `<handle>` to confirm".
- Step 3: **Delete account** stays disabled for anything but the exact handle.
- Step 4: the toast "Your account is deleted", then the browser signs out through `POST /logout`.
- Step 5: Lake refuses the email and password.

## Phone width

#### ACCT-028 · Account pages on a phone · P2
As: owner

1. At 390 px, open `/settings`, `/sessions`, `/tokens` and **New token**.

Expect:
- Settings tabs wrap or scroll without moving the page sideways; the Profile cards are one column.
- Sessions: "This session" fills the width; the table scrolls inside its box; device cards are one
  column.
- Tokens: the drawer fills the screen with **Cancel** and **Create token** visible.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `frontend/src/pages/account/SettingsPage.tsx:18-25,59-64` vs `components/Shell.tsx:545` | The account menu hides Passkeys under Clerk, but Settings always shows the Passkeys tab and the MFA card; `docs/guide/account.md` says both are not offered under Clerk | ACCT-003, ACCT-015 |
| 2 | `frontend/src/pages/account/IdentityPanel.tsx:87-88,117` | "Use my own identity" says it cannot be undone, but it runs on one press with no confirmation | ACCT-005 |
| 3 | `frontend/src/pages/account/TokensPage.tsx:153`, `SessionsPage.tsx:153,185` | Tokens, other sessions and devices are revoked with no confirmation, while Developer → Your Keys asks first (`pages/developer/parts.tsx:486`); `docs/guide/tokens.md:110` says "Press Revoke and confirm" | ACCT-009, ACCT-010, ACCT-022 |
| 4 | `frontend/src/pages/account/TokensPage.tsx:111-158` | The table has no Access, Last used or Created column; `docs/guide/tokens.md` says every page that makes keys lists them | ACCT-016 |
| 5 | `frontend/src/pages/account/routes.ts:18,20` vs `SessionsPage.tsx:69`, `TokensPage.tsx:57` | Registry titles "Sessions" and "Tokens" (the tab title) differ from the headings "Sessions and Devices" and "API Tokens" | ACCT-007, ACCT-016 |
| 6 | `frontend/src/pages/account/SessionsPage.tsx:119,167`, `TokensPage.tsx:99` | Empty-state headings "No other sessions", "No devices" and "No API tokens" are sentence case, against Title Case | ACCT-008, ACCT-016 |
| 7 | `docs/ui/settings.md` (route table) | Lists five Settings tabs without `identity`; the page has six | ACCT-002 |
