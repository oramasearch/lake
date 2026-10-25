// What the browser scripts share (docs/deployment/browser-testing.md): Playwright, a log with
// pass/fail checks, Clerk sign-in with a one-time ticket (lake lands a signed-in person on `/`),
// lake's /v1 capabilities called with the browser's own session, the first top-up through Stripe's
// test Checkout, and the person's first workspace started from Ask Orama's home (`/ask`).
//
// Playwright is not a dependency of lake. It is found through PLAYWRIGHT (a path to its index.mjs),
// then a `playwright` package on the module path, then the sibling orama-website-v2 checkout. Not
// C++: keep it out of git clang-format.
import fs from "node:fs";

export async function loadChromium() {
  const candidates = [
    process.env.PLAYWRIGHT,
    "playwright",
    new URL("../../../../../orama-website-v2/node_modules/playwright/index.mjs", import.meta.url).href,
    new URL("../../../orama-website-v2/node_modules/playwright/index.mjs", import.meta.url).href,
  ].filter(Boolean);
  for (const spec of candidates) {
    try {
      return (await import(spec)).chromium;
    } catch {}
  }
  console.error(`Playwright not found. Set PLAYWRIGHT to its index.mjs. Tried:\n  ${candidates.join("\n  ")}`);
  process.exit(1);
}

/** A log written to <out>/log.json after every step. `check` records a pass condition; any false
 * one makes `failed()` true and the script exit 1. */
export function logger(out) {
  fs.mkdirSync(out, { recursive: true });
  const log = [];
  let failures = 0;
  const note = (step, detail = {}) => {
    log.push({ step, time: new Date().toISOString(), ...detail });
    console.log(new Date().toISOString().slice(11, 19), step, JSON.stringify(detail));
    fs.writeFileSync(`${out}/log.json`, JSON.stringify(log, null, 2));
  };
  const check = (step, passed, detail = {}) => {
    note(step, { passed: !!passed, ...detail });
    if (!passed) failures += 1;
    return !!passed;
  };
  return { note, check, failed: () => failures > 0, failures: () => failures };
}

/** Numbered full-page screenshots in <out>. */
export function shooter(page, out) {
  let n = 0;
  return (name) => page.screenshot({ path: `${out}/${String(++n).padStart(2, "0")}-${name}.png`, fullPage: true }).catch(() => {});
}

/** The parsed command line every browser script takes: <base-url> <ticket-file> <output-dir> ... */
export function args(usage) {
  const [base, ticketFile, out, ...rest] = process.argv.slice(2);
  if (!base || !ticketFile || !out) {
    console.error(`usage: ${usage}`);
    process.exit(2);
  }
  if (/(^|\.)lake\.live$/.test(new URL(base).hostname) && !process.env.LAKE_E2E_ALLOW_PRODUCTION) {
    console.error("refusing to run against lake.live (production); set LAKE_E2E_ALLOW_PRODUCTION=1 if that is meant");
    process.exit(2);
  }
  return { base: base.replace(/\/$/, ""), ticketFile, out, rest };
}

/** A browser page. With LAKE_E2E_STATE naming an existing file, the page starts with the session
 * saved there by an earlier sign-in (see signIn), so several scripts share one ticket. */
export async function newPage(chromium, base) {
  const browser = await chromium.launch();
  const state = process.env.LAKE_E2E_STATE;
  const context = await browser.newContext({ viewport: { width: 1440, height: 900 }, ...(state && fs.existsSync(state) ? { storageState: state } : {}) });
  await context.grantPermissions(["clipboard-read", "clipboard-write"], { origin: base });
  const page = await context.newPage();
  return { browser, context, page };
}

/** Sign in through Clerk's own form, which consumes the ticket; lake exchanges Clerk's session for
 * its `lake_session` and lands on `/` (Quickstart, lake#1128). Answers the viewer. */
export async function signIn(page, base, ticketFile) {
  const state = process.env.LAKE_E2E_STATE;
  if (state && fs.existsSync(state)) {
    // A session saved by an earlier script: no ticket is spent.
    await page.goto(`${base}/`);
  } else {
    const ticket = fs.readFileSync(ticketFile, "utf8").trim();
    await page.goto(`${base}/sign-in?__clerk_ticket=${encodeURIComponent(ticket)}`);
  }
  await page.waitForURL((url) => url.origin === base && url.pathname === "/", { timeout: 90000 });
  await page.getByTestId("quickstart-page").waitFor({ timeout: 60000 });
  if (state && !fs.existsSync(state)) {
    // Keep the session for the next script. It is a credential: mode 600, delete it when done.
    await page.context().storageState({ path: state });
    fs.chmodSync(state, 0o600);
  }
  return await api(page, base, "viewer");
}

/** Call a lake capability with the browser's session, as the app does (frontend/src/lib/api.ts): a
 * read is GET with the input as a query string, an action is POST with JSON. Answers the `value`,
 * or throws with lake's error. `workspace` scopes the call under /w/<id>. */
export async function api(page, base, name, input = {}, { post = false, workspace = "" } = {}) {
  const scope = workspace ? `/w/${encodeURIComponent(workspace)}` : "";
  const url = `${base}${scope}/v1/${name}`;
  const response = post
    ? await page.request.post(url, { data: input, headers: { Accept: "application/json" } })
    : await page.request.get(url, {
        params: Object.fromEntries(Object.entries(input).filter(([, v]) => v !== undefined && v !== null && v !== "")),
        headers: { Accept: "application/json" },
      });
  const body = await response.json().catch(() => null);
  if (!body?.ok) {
    const error = new Error(`${name}: HTTP ${response.status()} ${JSON.stringify(body?.error ?? body).slice(0, 300)}`);
    error.status = response.status();
    error.code = body?.error?.code;
    throw error;
  }
  return body.value;
}

/** The text of every error banner and alert on the page, for the log. */
export async function errorText(page) {
  const texts = await page.locator("main [role=alert]").allInnerTexts().catch(() => []);
  return texts.join(" | ").replace(/\s+/g, " ").slice(0, 400);
}

/** A new account is blocked until its first top-up (Lake is prepaid, lake#1114). When the viewer's
 * balance is blocked, add the suggested amount on Billing → Add Funds and pay on Stripe's test
 * Checkout with Stripe's test card. Refuses anything but a test-mode session (`cs_test_`), so it
 * can never pay for real. */
export async function ensureFunds(page, base, { note, check }) {
  const viewer = await api(page, base, "viewer");
  const balance = viewer.balance ?? {};
  if (balance.state !== "blocked") {
    note("funds", { state: balance.state ?? "not billed" });
    return true;
  }
  await page.goto(`${base}/billing/add-funds`);
  await page.getByRole("button", { name: /^Add \$/ }).click();
  await page.waitForURL(/^https:\/\/checkout\.stripe\.com\//, { timeout: 60000 });
  if (!page.url().includes("/cs_test_")) {
    return check("funds", false, { error: "Stripe Checkout is not in test mode; refusing to pay" });
  }
  // Pay in US dollars (adaptive pricing may offer the local currency first).
  const usd = page.getByRole("button", { name: /^US \$/ });
  if (await usd.count()) await usd.click();
  await page.getByRole("textbox", { name: "Email" }).fill(viewer.email || "e2e@example.com").catch(() => {});
  await page.getByRole("textbox", { name: "Card number" }).fill("4242 4242 4242 4242");
  await page.getByRole("textbox", { name: "Expiration" }).fill("12 / 34");
  await page.getByRole("textbox", { name: /CVC/ }).fill("123");
  await page.getByRole("textbox", { name: "Cardholder name" }).fill("E2e Test");
  await page.getByRole("combobox", { name: "Country or region" }).selectOption("US");
  const zip = page.getByRole("textbox", { name: /ZIP/ });
  if (await zip.count()) await zip.fill("94103");
  // Stripe may offer to save the card with Link; leave that off.
  const link = page.getByRole("checkbox", { name: /Save my information/ });
  if ((await link.count()) && (await link.isChecked())) await link.uncheck();
  await page.locator("button[type=submit]").first().click();
  await page.waitForURL((url) => url.origin === base, { timeout: 120000 });
  // The credit arrives with Stripe's webhook, a moment after the redirect.
  let state = "";
  for (let i = 0; i < 30; i++) {
    state = (await api(page, base, "viewer")).balance?.state ?? "";
    if (state && state !== "blocked") break;
    await page.waitForTimeout(2000);
  }
  return check("funds", state && state !== "blocked", { state, topped_up: true });
}

/** The current workspace, ready. With none, start the first one with Ask Orama's home button
 * ("Start a workspace", lake#764, at `/ask` since lake#1128) and wait until Lake reports it ready.
 * Answers its id, or "". */
export async function ensureWorkspace(page, base, { note, check }, minutes = 10) {
  let viewer = await api(page, base, "viewer");
  if (!viewer.workspace_id) {
    await page.goto(`${base}/ask`);
    const start = page.getByRole("button", { name: "Start a workspace" });
    await start.waitFor({ timeout: 30000 });
    await start.click();
    note("start-workspace", {});
    for (let i = 0; i < 30 && !viewer.workspace_id; i++) {
      await page.waitForTimeout(2000);
      viewer = await api(page, base, "viewer");
    }
  }
  const workspace = viewer.workspace_id ?? "";
  const started = Date.now();
  let status = {};
  while (workspace && Date.now() - started < minutes * 60 * 1000) {
    status = await api(page, base, "workspace_status", { customer_id: viewer.customer_id }, { workspace }).catch((e) => ({ error: e.message }));
    if (status.ready || status.phase === "Failed") break;
    await page.waitForTimeout(10000);
  }
  check("workspace", !!status.ready, { workspace, phase: status.phase, seconds: Math.round((Date.now() - started) / 1000), error: status.error ?? status.message });
  return status.ready ? workspace : "";
}

/** A workspace page's address (frontend/src/routes.tsx workspacePath). */
export const inWorkspace = (base, workspace, path) => `${base}/w/${encodeURIComponent(workspace)}${path}`;

/** Wait until `test()` is true, polling every `every` ms; answers whether it became true. */
export async function until(page, test, { minutes = 5, every = 5000 } = {}) {
  const deadline = Date.now() + minutes * 60 * 1000;
  while (Date.now() < deadline) {
    if (await test().catch(() => false)) return true;
    await page.waitForTimeout(every);
  }
  return false;
}

/** An engine read in the workspace Zetta (`workspace_query {capability, input}`), as the app's
 * workspace pages call it (frontend/src/pages/design/api.ts). */
export const wq = (page, base, customer, workspace, capability, input = {}) =>
  api(page, base, "workspace_query", { customer_id: customer, workspace_id: workspace, capability, input }, { post: true, workspace });

/** An engine action in the workspace Zetta (`workspace_act {capability, input}`). */
export const wa = (page, base, customer, workspace, capability, input = {}) =>
  api(page, base, "workspace_act", { customer_id: customer, workspace_id: workspace, capability, input }, { post: true, workspace });
