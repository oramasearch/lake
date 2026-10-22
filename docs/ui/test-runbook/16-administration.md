# Administration

The operator's pages: Customers, Applications and an application's Users, Organizations and
Webhook events, Secrets as the operator, Repository Usage (`/metering`) and the Audit Log. It also
covers the pages around them that any persona reaches: Encryption Keys, the public Transparency
Monitor and the public Model page. Organization pages as the account holder are in
[15-organization.md](15-organization.md); Usage and Billing are in
[17-usage-and-billing.md](17-usage-and-billing.md).

## Before you start

- Personas: `operator`, `owner`, `member`, `visitor`.
- Test data: `W1`, `TD-ORG`, `TD-SECRET` (from [15-organization.md](15-organization.md)),
  `TD-REPO`.
- This file makes, and deletes at clean-up:
  - the application `qa-<MMDD>-app` in `owner`'s `W1` (ADM-006), its organization `qa-<MMDD>-apporg`
    and a webhook;
  - an application user signed up through the hosted sign-in with a test inbox address
    `qa-<MMDD>-appuser@<test inbox domain>`;
  - the keys `qa-<MMDD>-key` and `qa-<MMDD>-child` in `TD-SECRET`'s mount (keys have no delete:
    list them in the results file).
- Extra data for this file alone:
  - `TD-MODEL-PUBLIC`: a public model repository (namespace and name), and `TD-MODEL-PRIVATE`, a
    private one. Make them public or private with *API* `set_repository_visibility`.
  - An HTTPS URL that accepts POSTs (for the webhook), such as a request bin the team controls.
  - `member`'s TAAP identity (from `TD-ORG`'s **Members** tab).
- The operator signs in at `/login`. Never put the operator token in a result, a screenshot or an
  issue.

## Customers

#### ADM-001 · Customers lists every tenant · P0
As: operator

1. Open **Administration → Customers**.

Expect:
- Title "Customers", eyebrow "Administration", sub "The tenants. Each is an individual or an
  organization and owns its repositories."
- Figures **Customers**, **Individuals** and **Organizations** add up.
- Columns **Handle** (linked, the user id or organization id below), **Type** (chip `individual` or
  `organization`, the latter in the action colour) and **Created**.
- `owner`'s account and `TD-ORG`'s account are listed.

#### ADM-002 · Search, filter and sort customers · P1
As: operator

1. Type `qa-<MMDD>` into **Search customers…**, then `zzz-none`.
2. Clear it. Press the chip `organization`, then **Any type**.
3. Press **Handle**, **Type** and **Created**.

Expect:
- Step 1: the first search keeps `TD-ORG`'s account; `zzz-none` shows "No customers" with "Clear the
  search or the type filter to see more."
- Step 2: the chip narrows to organizations and shows its count.
- Step 3: each header sorts, ascending then descending.

#### ADM-003 · A customer's handle opens its repositories · P2
As: operator

1. Press `owner`'s handle.

Expect:
- The browser opens `/repositories?customer_id=<id>` listing `owner`'s repositories, `TD-REPO`
  among them.
- There is no link to the account's Usage or Billing on this page (suspected defect 1).

#### ADM-004 · An account opening Customers by its address · P2
As: owner

1. Open `/customers`.

Expect:
- The code opens the Customers page for any account (`pages/admin/routes.ts:7-9`): it lists only
  the customers `owner` can see, its own account and `TD-ORG`'s.
- G-103 expects "Not Available" here; record which one the page shows (suspected defect 2).

## Applications

#### ADM-005 · Applications with none registered · P2
As: operator (on a Lake with no application yet; skip otherwise)

1. Open **Administration → Applications**.

Expect:
- "No applications" with "An application gives your product hosted sign-in, its own user pool and
  signed lifecycle webhooks." and **Register application**.

#### ADM-006 · Register an application · P0
As: operator · Needs: W1

1. Open **Administration → Applications** and press **Register application**.
2. In **Workspace**, choose "<owner's handle> / W1".
3. Type `qa-<MMDD>-app` as **Name** and `https://example.com` on one line of **Allowed origins (one
   per line)**. Leave the rest. Press **Register**.

Expect:
- Drawer "Register Application"; **Workspace** has the help "The workspace holds the application's
  lifecycle events and webhook deliveries, and never changes."; **Accent color** starts at
  `#0b6e99`.
- The browser opens `/application?id=<new id>` titled "qa-<MMDD>-app" with the tabs **Overview**,
  **Users**, **Organizations** and **Webhook events**.

#### ADM-007 · Register refuses missing or bad fields · P2
As: operator

1. Press **Register application** and press **Register** with every field empty.
2. Fill **Name**, type `not a url` in **Allowed origins** and press **Register**.
3. Press **Cancel**.

Expect:
- Step 1: the browser marks **Name** and **Allowed origins** as required; nothing is sent.
- Step 2: Lake's refusal shows under **Allowed origins** (or above the form); nothing is created.

#### ADM-008 · The applications list · P1
As: operator · Needs: ADM-006

1. Open **Administration → Applications**.
2. Press the chips **Lake domain**, **Custom domain**, then **All**.
3. Type `qa-<MMDD>` into **Search applications…**, then `zzz-none`.

Expect:
- Figures **Applications**, **Custom domains** ("sign in on their own host") and **Workspaces**
  ("hold their events and users").
- `qa-<MMDD>-app`'s card shows **Workspace** "<owner's handle> / W1", **Users** "0 users", **Pool**
  and **Domain** "Lake domain", with **users**, **organizations** and **webhook events** below.
- Step 2: **Custom domain** shows "No application matches" with "Clear the search or the filter to
  see more."
- Step 3 narrows the same way.

#### ADM-009 · The application's overview · P1
As: operator · Needs: ADM-006

1. Open `qa-<MMDD>-app`.
2. Press the copy button on **Hosted sign-in**.
3. Press **Hosted sign-in** in the header.

Expect:
- Sub "Hosted sign-in, a user pool and signed lifecycle webhooks for one product."; tab title
  "Lake — Application: qa-<MMDD>-app".
- Card **Sign-in settings**: **Origins** shows `https://example.com` as a chip; **Redirect URIs**
  is empty.
- Link cards **Users** ("The application's user pool"), **Organizations** ("Its customers'
  organizations") and **Webhook events** ("Lifecycle events and webhooks").
- Card **Application**: **ID**, **Pool**, **Workspace**, **Custom domain** "—".
- Step 2 copies `https://<host>/apps/<id>/sign-in`.
- Step 3 leaves the app for the server-rendered sign-in page, in the application's accent colour.

#### ADM-010 · A user signs up through the hosted pages · P1
As: visitor, then operator · Needs: ADM-006

1. As `visitor`, open `/apps/<application id>/sign-up` and sign up with
   `qa-<MMDD>-appuser@<test inbox domain>`.
2. As `operator`, open the application's **Users** tab.

Expect:
- Title "qa-<MMDD>-app Users", sub "The people in this application's user pool."
- The user is listed under **User** with **Status** active and a **Suspend** button.

#### ADM-011 · Suspend an application user · P1
As: operator · Needs: ADM-010

1. Press **Suspend** on the user, then **Cancel**; press **Suspend** again, then **Suspend** in the
   dialog.

Expect:
- Dialog "Suspend the user?" with "qa-<MMDD>-appuser@… can no longer sign in to the application.
  There is no unsuspend."; the dialog's button is red.
- The status becomes `suspended` (red) and the row has no button.
- Signing in as that user on `/apps/<id>/sign-in` is refused.

#### ADM-012 · Search and filter the users · P2
As: operator · Needs: ADM-011

1. Press the status chip `suspended`, then **Any status**. Type `zzz-none` into **Search users…**.

Expect:
- The chip narrows the rows; `zzz-none` shows "No users" with "Clear the search or the status
  filter to see more."
- With no users at all, the text reads "Users appear here when they sign up through the
  application's hosted sign-in."

#### ADM-013 · Create and delete an application organization · P1
As: operator · Needs: ADM-006

1. Open the **Organizations** tab and check the empty state.
2. Press **New organization**, type `qa-<MMDD>-apporg` and `member`'s TAAP identity in **TAAP
   identity**; press **Create**.
3. Press **Delete** on it, then **Delete** in the dialog.

Expect:
- Step 1: "No organizations" with "Create the application's first organization." and **New
  organization**.
- Step 2: dialog "New Organization" with **Name** and **TAAP identity** required; toast
  "Organization created"; the row shows the TAAP identity below the name and **Updated**.
- Step 3: dialog "Delete the organization?" with "qa-<MMDD>-apporg and its memberships are deleted
  from the application."; no typed confirmation; the row goes away.
- The **Webhook events** tab now lists `organization.created` (green) and
  `organization.deleted` (red).

#### ADM-014 · Create a webhook; its secret shows once · P1
As: operator · Needs: ADM-006

1. Open **Webhook events** and press **New webhook**.
2. Type `http://example.com/hook` and press **Create**; then change it to your HTTPS test URL and
   press **Create**.
3. Copy the secret, press **Done**, and reload.

Expect:
- Drawer "New Webhook", subtitle "Its signing secret is shown once, after it is created."; **Events
  (comma-separated)** starts with
  `user.created,user.updated,user.deleted,organization.created,organization.updated,organization.deleted,session.started,mfa.changed`.
- Step 2: Lake refuses the `http://` URL (under **HTTPS URL** or above the form; record it if it is
  accepted); the HTTPS one closes
  the drawer and shows the green banner "Save this webhook secret now. It is not shown again." with
  a **Webhook secret** copy block and **Done**.
- Step 3: after **Done** and after the reload, the secret is nowhere on the page.
- Lifecycle events after this reach the test URL.

#### ADM-015 · The event log · P2
As: operator · Needs: ADM-013

1. On **Webhook events**, press the chip `organization`, then **All events**.
2. Type `deleted` into **Search events…**.

Expect:
- Title "qa-<MMDD>-app Event Log", sub "Signed lifecycle events, delivered to the application's
  webhooks."
- Columns **Event** (chip), **ID** and **Created**, newest first.
- Chips are event families with counts; the search narrows the rows.

#### ADM-016 · An account opening Applications by its address · P2
As: owner

1. Open `/applications`.

Expect:
- The page opens (the rail does not offer it to an account) and lists only applications in
  `owner`'s own accounts, `qa-<MMDD>-app` among them.

## Secrets as the operator

#### ADM-017 · Administration → Secrets · P1
As: operator · Needs: TD-SECRET

1. Open **Administration → Secrets**.
2. Open `/secret-mounts?workspace_id=<W1 id>`.

Expect:
- Title "Secret Mounts". Either a **Workspace** select over the workspaces the session knows, or,
  with none, "Choose a workspace" / "Choose a workspace to see its secret mounts." with **New
  mount** disabled.
- Step 2 lists `qa-<MMDD>-mount` with its owner, or shows the error "workspace_id must name one of
  your workspaces" when `W1` is not in the operator's session. Record which.

#### ADM-018 · The operator's New mount owner choices · P2
As: operator · Needs: ADM-017 shows a workspace

1. Press **New mount** and open **Owner** for `TD-ORGWS`, then for `W1`.
2. Press **Cancel**.

Expect:
- The operator is offered every organization whose space is the workspace's, as an administrator
  of every organization.
- Nothing is created.

## Encryption Keys

#### ADM-019 · A mount with no keys · P2
As: owner · Needs: TD-SECRET

1. On **Organization → Secrets**, press **Keys** on `qa-<MMDD>-mount`.

Expect:
- Title "qa-<MMDD>-mount Keys", sub "Named, versioned keys for encryption, signing, HMAC and
  derivation. Key material is never returned."
- Header **Static secrets** (opens the mount's secrets) and **New key**.
- "No keys yet" with "A key encrypts, signs and computes HMACs inside Lake; its material never
  leaves." and **New key**.
- `/encryption-keys` with no `mount_id` goes to `/secret-mounts`.

#### ADM-020 · Create a key · P0
As: owner · Needs: TD-SECRET

1. Press **New key**, type `qa-<MMDD>-key` and press **Create key**.

Expect:
- Dialog "New Encryption Key", subtitle "One key encrypts, signs (Ed25519) and computes
  HMAC-SHA-256; its material stays in Lake."; **Name** help "Lowercase letters, digits and
  dashes; unique in this mount."
- No **Derive from** field while the mount has no key.
- Toast "Created key qa-<MMDD>-key"; the row shows **Current version** 1, **Created**,
  **Updated**, **Try it** and **Rotate**. There is no Delete.
- Figures **Keys** 1, **Rotated** 0 ("past their first version"), **Derived** 0 ("children of
  another key").

#### ADM-021 · New key refuses a bad or taken name · P2
As: owner · Needs: ADM-020

1. Press **New key**. Type `Bad Key` and press **Create key**.
2. Type `qa-<MMDD>-key` and press **Create key**.

Expect:
- **Create key** is disabled while **Name** is empty.
- Step 1: the browser refuses the name format.
- Step 2: Lake's refusal shows under **Name** or above the form; no second key.

#### ADM-022 · Derive a child key · P1
As: owner · Needs: ADM-020

1. Press **New key**, type `qa-<MMDD>-child` and choose `qa-<MMDD>-key` in **Derive from**.
2. Leave **Context** empty, then type `eu`; press **Derive key**.

Expect:
- **Derive from** offers "Nothing: a new, independent key" and `qa-<MMDD>-key`.
- **Context** appears once a parent is chosen, with the help "Text that makes this child distinct,
  such as a region or tenant; sent as base64url."; **Derive key** is disabled while it is empty.
- Toast "Derived qa-<MMDD>-child from qa-<MMDD>-key"; the row reads "derived from <parent id>";
  **Derived** is 1.

#### ADM-023 · Rotate a key · P1
As: owner · Needs: ADM-020

1. Press **Rotate** on `qa-<MMDD>-key`, then **Cancel**; press **Rotate** again and **Rotate key**.

Expect:
- Dialog "Rotate qa-<MMDD>-key?" with "Version 2 of `qa-<MMDD>-key` becomes current: encrypt, sign
  and HMAC use it from now on. Older versions still decrypt and verify, and rewrap moves old
  ciphertext to version 2."
- Toast "qa-<MMDD>-key is now at version 2"; **Current version** 2; **Rotated** 1.

#### ADM-024 · Try it: encrypt and decrypt · P1
As: owner · Needs: ADM-020

1. Press **Try it** on `qa-<MMDD>-key`.
2. On **Encrypt**, type `hello qa` in **Plaintext** and press **Encrypt**.
3. Press **Decrypt** and press the **Decrypt** button.

Expect:
- Drawer "Try It" with the key name below; tabs **Encrypt**, **Decrypt**, **Rewrap**, **Sign**,
  **Verify**, **HMAC**; banner "Text → ciphertext under the current version. Every call is
  audited; what you type here is not kept."
- The run button is disabled while the input is empty.
- Step 2: a copy block "Encrypt (version <n>[, <algorithm>])" holding `lake:v1:…`.
- Step 3: **Ciphertext** is already filled; the answer is "Plaintext (version <n>)" reading
  `hello qa`.
- Closing and reopening the drawer starts empty.

#### ADM-025 · Try it: rewrap after a rotation · P2
As: owner · Needs: ADM-020

1. Encrypt `old` with the key and keep the ciphertext. Rotate the key as in ADM-023, then open **Try it** and choose
   **Rewrap**; paste the old ciphertext and press **Rewrap**.
2. Choose **Decrypt** and press **Decrypt**.

Expect:
- Step 1: the answer names the new current version.
- Step 2 gives `old`.

#### ADM-026 · Try it: sign and verify · P1
As: owner · Needs: ADM-020

1. Choose **Sign**, type `msg` in **Message**, press **Sign**.
2. Choose **Verify**, keep `msg` and the filled **Signature**, press **Verify**.
3. Change the message to `msg2` and press **Verify**.

Expect:
- Step 1: a copy block "Sign (version <n>, <algorithm>)".
- Step 2: the chip `valid` and "checked with version <n>".
- Step 3: the chip `invalid` (red).
- **Key version** has the help "Optional: the current version (<n>) when empty."

#### ADM-027 · Try it: HMAC and a key version · P2
As: owner · Needs: ADM-023

1. Choose **HMAC**, type `msg`, press **HMAC**. Then type `1` in **Key version** and press it again.
2. Type `99` in **Key version** and press **HMAC**.

Expect:
- Step 1: two different values, the first naming the current version, the second version 1.
- Step 2: Lake's refusal shows in the drawer.

#### ADM-028 · Try it: decrypt something not UTF-8 · P2
As: owner · Needs: ADM-020

1. Choose **Decrypt**, paste `garbage`, press **Decrypt**.

Expect:
- Lake's refusal shows above the button, with **Ciphertext**'s help "lake:v1:<key id>:<version>:<sealed>,
  as encrypt answers it." still visible.

#### ADM-029 · New key on a phone · P2
As: owner · Needs: TD-SECRET

1. At 390×844, open the mount's keys and press **New key**; tap **Name**.

Expect:
- A full-screen sheet with **Cancel** and **Create key** pinned above the keyboard.
- The keys table scrolls inside its own box; **Try it** and **Rotate** stay reachable.

## Repository Usage

#### ADM-030 · Repository Usage for your own account · P1
As: owner · Needs: W1

1. Open **Organization → Usage** and press the card **Repository Usage by Hour**.

Expect:
- `/metering?customer_id=<owner's account id>`; title "Usage" (suspected defect 3), sub "What you
  store and the compute you have used, measured from your repositories, container images and
  workspaces."
- The form **Customer ID** (filled) and **Show usage**; the header **Set storage limit**.
- Figures **Stored**, **Transfer** ("<x> up · <y> down, by the hours below"), **Builds** and **Run
  time**.
- Cards **Repository storage limit** (with **Change** and "New repository writes are refused above
  this limit; existing content stays readable."), **Estimated charge** (an amount, or "This Lake
  has no prices set, so there is no charge to show."), **Storage** (Repositories, Container
  images, Workspace data), **Compute** (Repository builds, Image builds, "Workspace runs, training
  included", Total).
- **Repository usage by hour** with **Hour** (`YYYY-MM-DD HH:MM UTC`), **Uploaded**,
  **Downloaded**, **Build Time**, and **Charge** only when priced; "No hourly usage." when empty.

#### ADM-031 · The operator chooses a customer · P1
As: operator

1. Open `/metering`.
2. Type `owner`'s account id in **Customer ID** and press **Show usage**.
3. Type `no-such-customer` and press **Show usage**.

Expect:
- Step 1: sub "What a customer stores and the compute it has used, measured from its repositories,
  container images and workspaces."; "Choose a customer" with "Enter a customer's ID to see what it
  stores and the compute it has used."; no **Set storage limit**.
- **Show usage** with an empty field is blocked by the browser.
- Step 2: the address carries `customer_id` and the figures load.
- Step 3: an error block with Lake's message.

#### ADM-032 · Set the repository storage limit · P1
As: owner · Needs: ADM-030

1. Press **Set storage limit**.
2. Type `1.5` and press **Set limit**; then type `-1`.
3. Type `10737418240` and press **Set limit**.
4. Open it again, type `0` and press **Set limit**.

Expect:
- Dialog "Repository Storage Limit", subtitle "New repository writes are refused above this limit;
  existing content stays readable."; field "Limit in bytes (0 for no limit)" with the current
  value.
- Step 2: "quota bytes must be a non-negative integer" under the field (or the browser's number
  check); nothing is sent.
- Step 3: the help under the field reads the size in words; toast "Limit set"; **Repository storage
  limit** shows the size with a meter.
- Step 4: the help reads "No limit."; the card reads "No limit".

#### ADM-033 · A member cannot set the limit · P2
As: member · Needs: TD-ORGWS

1. Open `/metering?customer_id=<TD-ORG's account id>`, press **Set storage limit** and save `0`.

Expect:
- The button is shown (suspected defect 4); Lake's refusal shows in the dialog; the limit is
  unchanged.

## Audit Log

#### ADM-034 · The Audit Log loads · P0
As: operator

1. Open **Administration → Audit**.

Expect:
- Title "Audit Log", eyebrow "Administration", sub "Signed, hash-linked events. Use the filters to
  narrow the evidence view."
- Figures **Events** ("newest first", "500+" at the cap), **Succeeded**, **Denied** and **Failed**.
- Columns **When**, **Action**, **Principal**, **Object**, **Outcome**, **Log**, **Digest**; events
  from every persona appear.
- There is no **Include reads** chip, and no "reads hidden" banner.

#### ADM-035 · Outcome chips and order · P1
As: operator

1. Press **Denied**, then **Failed**, then **Any outcome**.
2. Press **Newest first**, then **Oldest first**.

Expect:
- Each chip sets `?outcome=` and the rows hold only that outcome; the figure colours **Denied** and
  **Failed** red when non-zero.
- Step 2 toggles `?order=oldest` and back; the **Events** sub follows.

#### ADM-036 · The Filters form · P1
As: operator

1. Press **Filters**.
2. Type `member`'s TAAP identity in **Principal** and press **Filter**.
3. Add `organization:` in **Object**, a **From** a day ago in `2026-10-21T00:00:00Z` form, and press
   **Filter**.
4. Clear the fields and press **Filter**.

Expect:
- Fields **Principal**, **Object** (placeholder `repository/…`), **Outcome**, **From**, **To**,
  **Order**; no **Include reads** checkbox.
- Step 2: only `member`'s events; **Filters** is highlighted; the address carries `principal=`.
- Step 3 narrows further; reloading keeps the form open with the values.
- Step 4: the address has no filter parameters.

#### ADM-037 · A refused call is recorded · P1
As: member, then operator

1. As `member`, press **Invite people** on `TD-ORG` and send one (refused, [15-organization.md](15-organization.md) ORG-026).
2. As `operator`, filter the Audit Log by **Denied**.

Expect:
- A `denied` row with `create_invitation`, `member`'s name as **Principal** (identity on hover) and
  the **Log** chip `Central`.

#### ADM-038 · The Transparency log card · P2
As: operator

1. Scroll below the table.

Expect:
- When the Lake has a checkpoint service: card **Transparency log** with **Effective mode**,
  **Witness quorum** ("<t> of <n>"), **Aggregation** ("endpoint configured" or "—") and **API**
  `GET /v1/transparency_log_configuration`. Without one, no card.
- The footer "API: `GET /v1/audit`, `GET /v1/export_audit`, and `GET /v1/verify_audit`."

#### ADM-039 · Search, and a workspace that could not be read · P2
As: operator

1. Type `create_` into **Search these events…**, then `zzz-none`.

Expect:
- The search narrows the rows already read; `zzz-none` shows "No audit events" / "No event matches
  the search."
- A workspace whose audit cannot be read shows the warning "The audit of workspace `<id>` could not
  be read: <message>" above the figures, and the rest still lists.
- Pending events show "being written" in **Digest**.

#### ADM-040 · An account opening the Audit Log · P2
As: owner

1. Open `/audit`.

Expect:
- "Not Available" with "This page is for the Lake operator." and **Home**.

## Transparency Monitor

#### ADM-041 · Scan for an identity, signed out · P0
As: visitor · Needs: member's TAAP identity

1. Open `/transparency`.
2. Paste `member`'s TAAP identity and press **Scan**.

Expect:
- Step 1: eyebrow "Public evidence", title "Transparency Monitor", sub "Scan checkpoint-bound bulk
  tiles for every TAAP attestation that names an identity."; the field has the placeholder
  `taap:1:…`; "Scan for an identity" with "Enter a TAAP identity to verify the current checkpoint
  and scan its immutable tiles."
- No sign-in is asked.
- Step 2: `?identity=` in the address; figures **Checkpoint** ("entries"), **Tiles read**, **Scan**
  ("<x> ms" with the chip `verified`) and **Matches**; a **Checkpoint root** copy block.
- Card **Attestations naming this identity** lists rows with **Index**, **Profile**, **Issuer** and
  **Subject** (the membership attestation from ORG-007 among them).

#### ADM-042 · A scan with no match · P2
As: visitor

1. Scan `taap:1:qa-none`.

Expect:
- Either the figures with **Matches** 0 and "No attestations name this identity.", or an error
  block with Lake's message and **Retry** for an invalid identity. Record which.
- The footer "API: `GET /v1/transparency_identity_scan`, `GET /v1/transparency_tile`, and `GET
  /v1/transparency_checkpoint`."
- The tab title reads "Lake — Transparency" (suspected defect 5).

## Model

#### ADM-043 · A public model, signed out · P1
As: visitor · Needs: TD-MODEL-PUBLIC

1. Open `/models/<namespace>/<name>`.

Expect:
- Breadcrumb "Public Repositories › <namespace>/<name>"; title the model id; sub "Revision <sha>"
  and, with provenance, the chip `verified` or `not verified`.
- Figures **Logical bytes**, **Unique bytes**, **Deduplicated bytes** and **Deduplication ratio**
  (six decimals); hovering a byte figure shows the exact bytes.
- Card **Training provenance** with its rows, or "No training provenance is attached."
- Cards **Files** (a count; **Path**, **Size**, **Stored**, **Saved**) and **Revisions**
  (**Revision**, **Logical**, **Unique**, **Saved**), or "No files." / "No revisions."

#### ADM-044 · A private model is not found · P2
As: visitor, then owner · Needs: TD-MODEL-PRIVATE

1. As `visitor`, open `/models/<namespace>/<name>`.
2. As `owner`, open the same address, and `/models/<name>`.

Expect:
- Step 1: title "Model Not Found" with an error block and no **Retry**.
- Step 2: the model opens; the breadcrumb starts "Repositories".

#### ADM-045 · Search a model's files · P2
As: owner · Needs: a model with more than 6 files

1. Type part of a file name into **Search files…**; press **Size**.

Expect:
- The search box shows only when there are more than 6 files; it narrows the rows.
- **Size**, **Stored** and **Saved** sort.

## Phone

#### ADM-046 · Administration pages on a phone · P2
As: operator · Needs: ADM-006

1. At 390×844, open **Customers**, **Applications**, `qa-<MMDD>-app`'s **Webhook events** and the
   **Audit Log** with **Filters** open.

Expect:
- Application cards are one per row; the tables scroll inside their box.
- The Filters form wraps its fields; **Filter** stays reachable.
- **New webhook** opens a drawer that fills the screen.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `pages/admin/CustomersPage.tsx:45` | A customer row links only to its repositories; nothing in the app links to `/billing?customer_id=` or `/usage?customer_id=` from here. | ADM-003 |
| 2 | `pages/admin/routes.ts:7-11` vs `global-checks.md` G-103 | Customers and Applications open for any account, while G-103 expects an account to see "Not Available" on `/customers`. One of the two is wrong. | ADM-004 |
| 3 | `pages/admin/MeteringPage.tsx:25,29`; `pages/admin/routes.ts:21` | `/metering` is registered as "Repository Usage" and linked as "Repository Usage by Hour", but its title and tab title are "Usage", the same as `/usage`. | ADM-030 |
| 4 | `pages/admin/MeteringPage.tsx:36-40` | **Set storage limit** shows to any reader; `set_storage_quota` needs `admin` on the account. | ADM-033 |
| 5 | `pages/admin/TransparencyPage.tsx:13` | The tab title is "Lake — Transparency"; the page is "Transparency Monitor". | ADM-042 |
| 6 | `pages/admin/TransparencyPage.tsx:40` | The Scan figure always shows the chip `verified` once the read answers; it reads no verification field. | ADM-041 |
| 7 | `pages/admin/ApplicationsPage.tsx:181,227-233` | With no live workspace, **Workspace** is an empty select with no message, and **Register** sends an empty `workspace_id` and `customer_id`. | ADM-006 |
| 8 | `ApplicationUsersPage.tsx:15`, `ApplicationOrganizationsPage.tsx:17,75`, `ApplicationEventsPage.tsx:26,85` | An application with no workspace never loads its lists and shows the ordinary empty states; **New organization** and **New webhook** send an empty `workspace_id`. | ADM-012 |
| 9 | `pages/admin/AuditPage.tsx:156-163` | **From** and **To** are free text with no format hint or check. | ADM-036 |
