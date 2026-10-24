# The instance brand (white label)

A white-labelled Lake shows its customer's brand instead of Lake and Orama: in the console, on the
sign-in pages, on every page lake renders itself, in every email it sends and on its agent
surfaces. Private Warehouse, Dedicated Cluster and On-Prem sell it ("Your own domain,
white-labelled", lake#1129). This page is lake's half: the brand setting and where it is read
(lake#1135, lake#1137). The customer's domain and its TLS are the operator's
(orama-operator#947); sign-in on that domain is lake#1136.

**The default is Lake's own brand, and with it nothing changes.** Every surface below is
byte-for-byte what it was before the brand existed: the same email, the same `llms.txt`, the same
skill at `/skills/lake/SKILL.md`, the same pages. `tests/test_brand.cpp` checks this.

## The setting

The brand is instance configuration, one per Lake, never workspace data and never stored. The
operator renders it as environment variables, a mounted JSON document, or both. Lake reads it once
at start (`brand_from_sources`, `include/lake/api/brand.hpp`) and stops with exit status 2 and a
message naming the setting when it is invalid.

`LAKE_BRAND_FILE` names a JSON object with the keys in the second column. Each `LAKE_BRAND_*`
variable that is set wins over the file's value. A relative image path in the file is read
against the file's directory, so a ConfigMap can hold `brand.json` beside `logo.svg`.

| Variable | File key | Default (Lake) | What it is |
|---|---|---|---|
| `LAKE_BRAND_NAME` | `name` | `Lake` | The product's name in prose, titles, headings and email. **Required** for a custom brand. 1 to 64 characters, none of `< > & " \ ` * [ ]` |
| `LAKE_BRAND_SHORT_NAME` | `short_name` | `lake`; for a custom brand, the name lowercased with hyphens (`Acme Data` → `acme-data`) | The name in identifiers: the skill's path and name (`/skills/<short_name>/SKILL.md`), MCP `serverInfo.name`, `server_info`'s `name`, the `claude mcp add` name and the key variable (`$ACME_DATA_KEY`). Lowercase letters, digits and hyphens, starting with a letter, at most 32 |
| `LAKE_BRAND_COMPANY` | `company` | `Orama`; custom: the name | Who ships the catalogues and the library agents ("Acme's AI solutions") |
| `LAKE_BRAND_ASSISTANT_NAME` | `assistant_name` | `Ask Orama`; custom: `Ask <name>` | The assistant beside every page and the home |
| `LAKE_BRAND_CONSOLE_TITLE` | `console_title` | `Lake Console`; custom: `<name> Console` | The console's document title and label |
| `LAKE_BRAND_BY_ORAMA` | `by_orama` | `true`; custom: `false` | Whether the eyebrow reads "<name> by Orama" and the wordmark shows "Orama" |
| `LAKE_BRAND_LOGO` | `logo` | Lake's mark | The logo: a base64 `data:image/(png\|svg+xml\|x-icon\|webp\|jpeg\|gif)` URI, or a path to such a file (at most 256 KiB), which lake inlines as one |
| `LAKE_BRAND_LOGO_DARK` | `logo_dark` | the logo | The logo in the dark theme |
| `LAKE_BRAND_FAVICON` | `favicon` | none | The browser icon, as the logo |
| `LAKE_BRAND_ACCENT` | `accent` | the design tokens' action blue | The action colour, `#rrggbb` |
| `LAKE_BRAND_ACCENT_DARK` | `accent_dark` | the accent | The action colour in the dark theme; needs `LAKE_BRAND_ACCENT` |
| `LAKE_BRAND_SUPPORT_EMAIL` | `support_email` | `LAKE_SUPPORT_EMAIL` (`support@orama.com`) | The address people are told to write to, and the Reply-To of customer email. **Required** for a custom brand, and not on `orama.com`, `lake.live` or `oramadev.net` |
| `LAKE_BRAND_EMAIL_FROM` | `email_from` | `LAKE_RESEND_FROM` (`Lake <orders@lake.live>`) | The From of every email, `Name <address>` or an address. **Required** for a custom brand (unless `LAKE_RESEND_FROM` already names the customer's domain), and not on those domains. Its domain must be verified in Resend: SPF and DKIM records in the customer's zone |
| `LAKE_BRAND_LEGAL_NAME` | `legal_name` | none | The legal name in the footer of the pages and the sign-in screens (`© Acme Data Inc.`) |
| `LAKE_BRAND_TERMS_URL` | `terms_url` | none | An `https` link to the terms, in the same footer |
| `LAKE_BRAND_PRIVACY_URL` | `privacy_url` | none | An `https` link to the privacy notice, in the same footer |

Setting any of them makes a **custom brand**; Lake then refuses to start unless `LAKE_BRAND_NAME`,
`LAKE_BRAND_SUPPORT_EMAIL` and a sender off Lake's domains are given, so a half-configured white
label never sends a customer's users email from `lake.live` or tells them to write to Orama.
Lake's startup line says `"brand": "default"` or the brand's name.

An example `brand.json`:

```json
{
  "name": "Acme Data",
  "short_name": "acme-data",
  "company": "Acme",
  "logo": "logo.svg",
  "logo_dark": "logo-dark.svg",
  "favicon": "favicon.ico",
  "accent": "#aa3300",
  "support_email": "help@acme.example",
  "email_from": "Acme Data <hello@acme.example>",
  "legal_name": "Acme Data Inc.",
  "terms_url": "https://acme.example/terms",
  "privacy_url": "https://acme.example/privacy"
}
```

## Where it is read

The server installs the brand as the process's `instance_brand()` when it is constructed
(`ServerOptions::brand`), so every surface renders it without a parameter of its own.

- **The console** (the web app, no build per brand). `viewer` answers `brand` (name, short name,
  company, assistant name, console title, eyebrow, `by_orama`, the images, colours, support address
  and legal footer). index.html already carries the `viewer` answer, and for a custom brand lake
  also writes the console title into `<title>` and the favicon and colour overrides into `<head>`,
  so the first paint is the brand's. The rail's wordmark, the top bar's assistant button, the
  rail's Home item, the document titles ("Acme Data — Projects"), the account menu's operator name,
  the version line, the Ask panel and the sign-in frame (logo, name, kicker, legal footer) read it
  (`frontend/src/lib/brand.ts`, `components/BrandMark.tsx`).
- **Sign-in.** The sign-in frame above, the Clerk page's eyebrow and lines, and Clerk's
  `appearance` colour (the brand's accent). Clerk's own "Secured by Clerk" depends on the Clerk
  plan; sign-in on the customer's domain is lake#1136.
- **Colours.** One accent sets the design system's action tokens (`--orama-action-default`,
  `-filled`, `-hover`, `-pressed`, `-soft`, `-focus`; hover and pressed are mixed from it) in the
  same three selectors as the generated token layer, light and dark. It overrides the tokens at run
  time; the generated files are unchanged, so `tools/ui/test-token-gates.sh` still holds. This is
  the same approach as an application's theme (`update_application_theme`), at the instance level.
- **Pages lake renders itself** (`web_ui.cpp`): the error page, the region picker and the apex
  order sheet's frame take the brand's name in titles, header, eyebrow and copy, its logo in the
  header, its favicon and colours in `<head>`, and its legal footer. An identity application's
  hosted pages keep that application's own theme, as before.
- **Email.** The order and balance emails are written in Lake's words and rewritten in the
  brand's (`Brand::apply`); the From is `LAKE_BRAND_EMAIL_FROM`, the Reply-To and the "write to"
  address the brand's support address. Order forms still go to `LAKE_SUPPORT_EMAIL`.
- **Agent surfaces** (lake#1137). `llms.txt`, `llms-full.txt` and the skill use the brand's name,
  link the skill at `/skills/<short_name>/SKILL.md` (lake also keeps answering
  `/skills/lake/SKILL.md`), and name the MCP server and key variable after the short name. Their
  links already follow `LAKE_PUBLIC_HOST` (`public_origin`). MCP `initialize` answers
  `serverInfo` `{name: <short_name>, title: <name>}` and `instructions` naming the brand; every
  tool's title and description in `tools/list`, and every title, summary and description of the
  OpenAPI document, are rewritten. `server_info`'s `name` is the short name. "lake.live is
  prepaid" becomes "<name> is prepaid when billing is on".

### What the rewriting changes, and what it leaves

`Brand::apply` (and `brandText` in the console, the same rules) rewrites lake's own copy, never a
person's or an application's content: "Lake by Orama" → the eyebrow, "Lake Console" → the console
title, "Ask Orama" → the assistant name, "the Orama operator" → "the operator", "Orama" → the
company, and "Lake" as a whole word → the name. Identifiers are not names and stay: `LakeSet`,
`lake_session`, `LAKE_*` variables, and the API's wire values such as `source: orama`,
`agent-ask-orama` and `oramacore_fulltext`. JSON schemas keep their keys and enum values; only
their `title`, `summary` and `description` strings change.

## What is not branded yet

- Pages of the console other than the frame, sign-in and Ask still have Lake or Orama in their own
  copy (Design → Solutions' "Orama's catalogue", the agent pages, Developer → MCP's help, the Docs).
  Follow-up: lake#1170.
- Runtime error messages ("ask Orama") and stored data (the Agent Library's agent names) are not
  rewritten.
- The operator half: rendering the variables above from the Lake resource, and the sender domain's
  DNS records. orama-operator#947.
- `lake manifest` (lake PR #1147, not merged) should declare every `LAKE_BRAND_*` variable above
  when it lands (lake#936).

## How to verify

```sh
LAKE_TEST_SKIP_STALE_SWEEP=1 build/tests/lake-tests --gtest_filter='Brand*'
(cd frontend && npm test)
```

`BrandConfig.*` covers the defaults, the file and the environment, and every refusal.
`BrandEmail.*`, `BrandPages.*` and `BrandAgentDocs.*` render each email, page and agent surface
with Lake's brand (unchanged) and with a test brand, and find no "Lake", "Orama" or `lake.live`
left. `BrandStartup.*` starts the binary: an invalid brand exits 2, a custom one is served on the
console's first paint, `llms.txt`, the skill, MCP and `server_info`. `frontend/tests/brand.test.ts`
checks the console's rewriting and that the shell, sign-in and Ask name nothing themselves.

By hand: start lake with `LAKE_BRAND_NAME`, `LAKE_BRAND_SUPPORT_EMAIL`, `LAKE_BRAND_EMAIL_FROM` and
`LAKE_BRAND_ACCENT`, open `/` and `/sign-in`, and `curl` `/llms.txt`, `/skills/<short>/SKILL.md`
and `POST /mcp` `initialize`.
