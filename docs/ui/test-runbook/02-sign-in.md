# Sign-In

The pages that sign a person in or make an account: Sign In and Create Account under Clerk, Lake's
own sign-in and sign-up (password, OAuth, passkey), multi-factor authentication, and the operator's
sign-in. They are bare pages: no rail and no top bar, only the sign-in frame.

## Before you start

- Personas: `owner` and `visitor` on a Clerk Lake (testing or dev); `operator` on any Lake.
- Most cases here run on `local` (Lake without Clerk). They are marked *Needs: `local`*. On a Clerk
  Lake `/sign-up` shows Clerk's form and there is no password, OAuth or MFA step.
- This file makes the local account other files use:
  - `TD-LOCAL`: `qa-<MMDD>-local@<test inbox domain>`, display name `QA Local`, password at least
    12 characters, made in AUTH-011. [19-account-settings.md](19-account-settings.md) uses it.
- A TOTP authenticator app and a passkey-capable browser for the MFA and passkey cases.
- For OAuth, a provider configured on `local` (`github` or `google`); without one, AUTH-027 is
  `blocked`.
- For the identity proof case, `liquid` on the tester's machine (`liquid identity show --document`,
  `liquid sign artifact`).
- The region cases need a Lake whose `server_info.region` is set.
- Each bare page has the same frame; AUTH-001 checks it once.

## The sign-in frame

#### AUTH-001 · The frame of a sign-in page · P1
As: visitor

1. Open `/sign-in`.
2. Press the theme button at the top right.
3. Press the "Orama" brand.

Expect:
- No rail and no top bar. A 2px brand strip, a "Skip to content" link on the first Tab, and a
  brand panel: the mark and "Orama", "Lake", "Design, train and ship AI on your own data, in your
  own workspace." and the list "Design", "Train", "Ship".
- Step 2: the theme changes; the button's name is "Use dark theme" or "Use light theme".
- Step 3: the landing at `/` opens.

#### AUTH-002 · A signed-in person is sent on · P1
As: owner, then operator

1. Signed in, open `/sign-in`, `/sign-up` and `/passkey-sign-in`.
2. Open `/sign-in?next=/search`.
3. As `operator`, open `/login`.

Expect:
- Step 1: each page leaves at once (nothing is drawn) for `/`.
- Step 2: the browser lands on `/search`.
- Step 3: the operator lands on `/repositories`.

## Sign in under Clerk

#### AUTH-003 · Sign in with Clerk · P0
As: owner (signed out) · Needs: a Clerk Lake

1. Open `/sign-in`.
2. Sign in with a ticket or the emailed code.

Expect:
- Step 1: eyebrow "Lake by Orama", title "Sign In", sub "Sign in with the email address you were
  invited with.", and Clerk's form inside the card (no Clerk header, Lake's blue).
- Step 2: the status line reads "Signing you in." and the browser goes through
  `POST /identity/clerk-session` to `/`, the Ask Orama home.

#### AUTH-004 · Sign-in returns to the page asked for · P1
As: owner (signed out)

1. Open `/sign-in?next=%2Fdashboard%3Fws%3Dx`.
2. Sign in.

Expect:
- The browser lands on `/dashboard?ws=x`.

#### AUTH-005 · A foreign return is refused · P1
As: owner (signed out)

1. Open `/sign-in?next=//example.com` and sign in.
2. Sign out, open `/sign-in?next=https://example.com` and sign in.
3. Sign out, open `/sign-in?return_to=/settings` and sign in.

Expect:
- Every step lands on `/`; the browser never leaves the Lake's origin.

#### AUTH-006 · Create Your Account under Clerk · P1
As: visitor · Needs: a Clerk Lake

1. Open `/sign-up`.

Expect:
- Title "Create Your Account", sub "Accounts are added by invitation while Lake is in development.
  Ask the person who invited you, or sign in if you already have access."
- Clerk's sign-up form, whose sign-in link returns to `/sign-in`.

#### AUTH-007 · The server's error is shown · P2
As: visitor · Needs: a Clerk Lake

1. Open `/sign-in?error=Test%20message`.

Expect:
- A red notice "Test message" above Clerk's form.

#### AUTH-008 · Signing out of Clerk does not sign straight back in · P1
As: owner · Needs: a Clerk Lake

1. Signed in, use **Sign out** in the account menu.
2. Read the sign-in page it lands on.

Expect:
- Clerk's form shows, signed out; the page does not sign the person in again on its own.
- The address no longer holds `signed_out`.

#### AUTH-009 · Clerk cannot load · P2
As: visitor · Needs: a Clerk Lake

1. In the Network panel, block requests to the Clerk Frontend API host.
2. Open `/sign-in`.

Expect:
- The status line reads "Sign-in could not load: Clerk could not be reached."

## Regions on the sign-in pages

#### AUTH-010 · The region sentence · P1
As: visitor · Needs: a Lake with a region

1. Open `/sign-in`, then `/sign-up`.

Expect:
- `/sign-in`: "You are signing in to Lake in <label>. An account belongs to the region it was
  created in." followed, when there are other regions, by "Your account is in another region? Go to
  <Label> (<host>)." with each host linked to `https://<host>/`.
- `/sign-up`: "Your account is created in <label>. Each region is a separate Lake with its own
  accounts, and your workspaces stay in <label>." followed by "To create it elsewhere, go to …".
- On a Lake without a region neither sentence shows.

## Lake's own sign-up

#### AUTH-011 · Create an account with a password · P0
As: visitor · Needs: `local`

1. Open `/sign-up`.
2. Enter **Email** `qa-<MMDD>-local@…`, **Display name** `QA Local`, a **Password** of 11
   characters, keep **Device name** "browser", and press **Create account**.
3. Make the password 12 characters or more and press **Create account**.

Expect:
- Step 1: eyebrow "identity", title "Create an Account", sub "Get a personal customer you own, then
  create your first repository.", and the notice "Lake gives your account a managed key, so you need
  nothing else to start. To sign with your own liquid identity instead, open Account → Identity
  after you sign in." Email, Password and Device name carry " *".
- Step 2: the browser refuses to submit (the field's minimum is 12).
- Step 3: the button spins, then the browser lands on `/repositories`, signed in as `QA Local`.

#### AUTH-012 · Sign-up refuses a taken email · P1
As: visitor · Needs: `local`, TD-LOCAL

1. Sign out. On `/sign-up`, enter the same email as AUTH-011 and a valid password.
2. Press **Create account**.

Expect:
- A red notice with Lake's message (on Postgres "another account has this email"); when Lake names
  the field, the same message also sits under **Email** (see Suspected defects 2).
- No second account is made; the page stays on `/sign-up` with the form filled.

#### AUTH-013 · Required fields on sign-up · P2
As: visitor · Needs: `local`

1. On `/sign-up`, clear **Device name** and press **Create account**.
2. Leave **Email** empty and press **Create account**.

Expect:
- The browser asks for each required field and nothing is sent.
- **Display name** is optional.

#### AUTH-014 · Links between sign-in pages keep the return · P2
As: visitor · Needs: `local`

1. Open `/sign-in?next=%2Fsearch`.
2. Press **Create an account**, then on that page **Sign in**.
3. Press **Verify TOTP**, go back, press **Use a passkey**.

Expect:
- Every link keeps `?next=%2Fsearch` and drops any other parameter.
- Step 2: the sign-up foot reads "Already registered? Sign in.".

## Lake's own sign-in

#### AUTH-015 · The native sign-in page · P1
As: visitor · Needs: `local`

1. Open `/sign-in`.

Expect:
- Eyebrow "identity", title "Sign In", sub "Use a password, passkey, or configured social provider."
- Panel "Password" with **Email**, **Password**, **Device name** (prefilled "browser"), **Sign in**,
  and the links "Verify TOTP" and "Use a passkey".
- Panel "OAuth" with **Provider** (placeholder "github or google"), **Device name**, "TAAP/1
  identity document for first sign-in", and **Continue to provider**.
- Foot: "Create an account" · "Operator sign-in".

#### AUTH-016 · Wrong password, then the right one · P1
As: TD-LOCAL (signed out) · Needs: `local`

1. Enter the email and a wrong password; press **Sign in**.
2. Enter the right password; press **Sign in**.

Expect:
- Step 1: a red notice with Lake's message (on Postgres "invalid email or password"); the form keeps
  what was typed.
- Step 2: the browser lands on `/`, the Ask Orama home.

## Multi-factor authentication

#### AUTH-017 · The MFA page when signed in · P1
As: TD-LOCAL · Needs: `local`

1. Open `/mfa` (or **Settings → Profile → Manage MFA**).

Expect:
- Breadcrumb Account › MFA. Eyebrow "Account", title "Multi-Factor Authentication", sub "Enroll or
  replace a TOTP authenticator for password sign-in. A new enrollment replaces existing recovery
  codes."
- Card "Authenticator" / "TOTP authenticator" with the chip "Not enrolled", the text "Add the
  authenticator URI to your authenticator app, and keep the recovery codes somewhere safe." and
  **Enroll or replace MFA**.
- Side card "Disable MFA" with **Current TOTP or recovery code** and **Disable MFA**.

#### AUTH-018 · Enroll MFA · P1
As: TD-LOCAL · Needs: `local`

1. Press **Enroll or replace MFA**.
2. Copy **Authenticator URI** into the authenticator app; write down two recovery codes.
3. Reload the page.

Expect:
- Step 1: a green banner "Save these now. They are not shown again." with a copy block
  "Authenticator URI" (an `otpauth://` URI), the label "Recovery codes · each code works once", a
  list of codes, and a copy block "All recovery codes". **Copy** reads "Copied" for a moment.
- After step 1 the chip reads "Enabled".
- Step 3: the banner is gone and the codes are not shown again.

#### AUTH-019 · Sign in with a TOTP code · P1
As: TD-LOCAL · Needs: `local`, AUTH-018

1. Sign out. On `/sign-in`, enter the email and password and press **Sign in**.
2. Enter `000000` in **TOTP or recovery code** and press **Verify and sign in**.
3. Press **Back**, then **Sign in** again.
4. Enter the code from the app and press **Verify and sign in**.

Expect:
- Step 1: the Password panel changes to "Verify your password with a current TOTP or unused
  recovery code." with **TOTP or recovery code** focused, **Verify and sign in** and **Back**.
- Step 2: a red notice with Lake's message (e.g. "invalid TOTP code"); the panel stays on the code
  step.
- Step 3: **Back** returns to the email and password form, code and error cleared.
- Step 4: the browser lands on `/`.

#### AUTH-020 · A recovery code works once · P2
As: TD-LOCAL · Needs: `local`, AUTH-018

1. Sign out; sign in with the password and one recovery code.
2. Sign out; sign in with the same recovery code.

Expect:
- Step 1 signs in.
- Step 2 is refused with a red notice.

#### AUTH-021 · The MFA challenge page for a visitor · P2
As: TD-LOCAL (signed out) · Needs: `local`, AUTH-018

1. Open `/mfa`.
2. Fill **Email**, **Password**, **TOTP or recovery code**, keep **Device name**, and press
   **Verify and sign in**.

Expect:
- Step 1: inside the shell with an empty breadcrumb; eyebrow "Account", title "Multi-Factor
  Authentication", sub "Verify your password with a current TOTP or unused recovery code."
- Step 2: the browser lands on `/`.

#### AUTH-022 · Disable MFA: wrong code, then the right one · P1
As: TD-LOCAL · Needs: `local`, AUTH-018

1. On `/mfa`, type `000000` in **Current TOTP or recovery code** and press **Disable MFA**.
2. Type the current code and press **Disable MFA**.
3. Reload.

Expect:
- Step 1: a red notice with Lake's message; MFA stays on.
- Step 2: the toast "MFA disabled". No dialog asks first.
- Step 3: the chip reads "Not enrolled".

#### AUTH-023 · The operator has no MFA page · P2
As: operator

1. Open `/mfa`.

Expect:
- The browser is sent to `/repositories`.

## Passkey sign-in

#### AUTH-024 · Register a passkey to sign in with · P1
As: TD-LOCAL · Needs: `local`, a passkey-capable browser

1. Open **Account menu → Passkeys**, keep or set **Passkey name** to `qa-<MMDD>-key`, press
   **Register passkey** and complete the browser's prompt.

Expect:
- A green banner "Passkey registered." ([19-account-settings.md](19-account-settings.md) covers the
  page in full).

#### AUTH-025 · Sign in with a passkey · P1
As: TD-LOCAL (signed out) · Needs: `local`, AUTH-024

1. Open `/sign-in` and press **Use a passkey**.
2. Keep **Device name** "browser" and press **Use passkey**; choose the passkey in the browser's
   prompt.

Expect:
- Step 1: eyebrow "identity", title "Sign In with a Passkey", a **Device name** field and **Use
  passkey**.
- Step 2: the button is disabled while the prompt is open, then the browser lands on `/`.

#### AUTH-026 · Cancel the passkey prompt · P2
As: TD-LOCAL (signed out) · Needs: `local`

1. On `/passkey-sign-in`, press **Use passkey** and cancel the browser's prompt.

Expect:
- The status beside the button shows the browser's message (e.g. "The operation either timed out
  or was not allowed."); the button is enabled again.

## OAuth

#### AUTH-027 · Continue to a provider · P1
As: visitor · Needs: `local` with an OAuth provider

1. On `/sign-in`, in **OAuth**, type the provider (e.g. `github`), keep **Device name**, and press
   **Continue to provider**.

Expect:
- The browser goes to the provider's sign-in page; after it, Lake's callback signs the person in
  and lands on `/`.

#### AUTH-028 · An unknown provider is refused · P2
As: visitor · Needs: `local`

1. Type `nosuchprovider` in **Provider** and press **Continue to provider**.

Expect:
- A red notice with Lake's message ("OAuth provider is not registered"); the page stays.

#### AUTH-029 · A first sign-in with an identity document proves its key `v0.4.0+` · P2
As: visitor · Needs: `local` with an OAuth provider, `liquid`

1. Paste the output of `liquid identity show --document` into "TAAP/1 identity document for first
   sign-in".
2. Press **Continue**.
3. Type `hello` into **What liquid sign printed**.
4. Press **Start again**; press **Continue** again; run the shown command and paste everything it
   prints; press **Continue to provider**.

Expect:
- Step 1: the button text changes from "Continue to provider" to "Continue".
- Step 2: "Prove you hold this identity's key: sign this challenge before it expires, and paste
  everything the command prints.", a copy block "Run" with `liquid sign artifact <challenge>`, and
  **What liquid sign printed**. The document box is read-only.
- Step 3: **Continue to provider** stays disabled (the text is not a signature).
- Step 4: **Start again** clears the challenge; after the paste the button is enabled and the
  browser goes to the provider.

## Operator sign-in

#### AUTH-030 · The operator sign-in page · P2
As: visitor

1. Open `/login`.
2. Press "sign in to your account".

Expect:
- Step 1: eyebrow "Operator access", title "Operator Sign In", sub "Use your operator credential to
  manage this Lake. For your own repositories, sign in to your account.", a password field
  "Operator credential" (placeholder "Operator credential") and **Sign in**.
- Step 2: `/sign-in` opens.

#### AUTH-031 · A wrong operator credential · P1
As: visitor

1. On `/login`, type `wrong` and press **Sign in**.
2. Press **Sign in** with the field empty.

Expect:
- Step 1: the page reloads at `/login?error=…` with a red notice "That token did not match."
- Step 2: the browser asks for the field; nothing is sent.

#### AUTH-032 · Sign in as the operator · P0
As: operator

1. On `/login`, enter the admin token and press **Sign in**.

Expect:
- The browser lands on the server's redirect; the rail foot reads "Lake operator" / "Operator" and
  the rail shows the Administration group.
- The token is never shown in the address or the page.

## Phone width

#### AUTH-033 · Sign-in pages on a phone · P2
As: visitor

1. At 390 px, open `/sign-in`, `/sign-up` and `/login`.

Expect:
- The brand panel sits above the card; the form fills the width; the theme button stays reachable
  at the top right.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `frontend/src/pages/account/AuthFrame.tsx:15` with `SignInPage.tsx:28`, `SignUpPage.tsx:54`, `ClerkSignIn.tsx:171`, `LoginPage.tsx:14`, `PasskeySignInPage.tsx:29` | Tab titles are lowercase ("Lake — sign in", "Lake — sign up", "Lake — create account", "Lake — operator sign in", "Lake — passkey sign in"), against G-01's Title Case | AUTH-003, AUTH-011, AUTH-025, AUTH-030 |
| 2 | `frontend/src/pages/account/SignInPage.tsx:84,111`, `SignUpPage.tsx:57`, `MfaPage.tsx:67,146` | When Lake's error names a field, the message shows twice: in the red notice and under the field | AUTH-012, AUTH-016, AUTH-019 |
| 3 | `frontend/src/pages/account/routes.ts:7` vs `SignUpPage.tsx:55`, `ClerkSignIn.tsx:174` | `/sign-up` is "Create Account" in the registry, "Create an Account" on Lake's page and "Create Your Account" under Clerk | AUTH-006, AUTH-011 |
| 4 | `frontend/src/pages/account/MfaPage.tsx:26,64` | The breadcrumb says "MFA" while the page title is "Multi-Factor Authentication" | AUTH-017 |
| 5 | `frontend/src/pages/account/PasskeySignInPage.tsx:1-2` | The comment says it lands on Sessions; it lands on `/` | AUTH-025 |
| 6 | `docs/ui/sign-up-walkthrough.md` steps 1-2, `docs/ui/identity.md` "First account" | Both describe an identity-document field and an Install liquid link on `/sign-up`; the page has neither | AUTH-011 |
