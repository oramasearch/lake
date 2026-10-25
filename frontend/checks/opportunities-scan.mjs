// lake#1195: a workspace scan survives leaving the page. Opens Design → Opportunities with no
// `?task=` while the workspace's scan runs (list_opportunities answers `last_scan` running), checks
// the page says "Scanning… started …" and the Scan button says it is running, then lets the scan
// finish and checks the results appear on the same page without a reload. A second press while a
// scan runs is not possible (the button is disabled); lake joins one that arrives anyway
// (agent_turn `joined`). Run against `npx vite --port 5174`; every /v1 call is answered by the
// mocks below, so no lake is needed. Playwright is not a dependency of the app:
//
//   PLAYWRIGHT_FROM=/path/to/a/project/with/playwright/package.json OUT=/tmp/scan node checks/opportunities-scan.mjs
//
// It writes running.png and finished.png to OUT and exits non-zero on any failed check.
import { createRequire } from "node:module";
import { mkdirSync } from "node:fs";
const require = createRequire(process.env.PLAYWRIGHT_FROM ?? import.meta.url);
const { chromium } = require("playwright");

const OUT = process.env.OUT ?? "/tmp/lake-opportunities-scan";
mkdirSync(OUT, { recursive: true });
const BASE = process.env.BASE ?? "http://localhost:5174";

const viewer = {
  signed_in: true,
  administrator: false,
  display_name: "Steve Sperandeo",
  email: "steve@example.com",
  customer_id: "c1",
  workspace_id: "ws1",
  spaces: [{ id: "c1", name: "steve", label: "Steve", personal: true, workspaces: [{ id: "ws1", name: "Main", customer_id: "c1", status: "ready" }] }],
  clerk: { enabled: false },
  password_sign_up: false,
  version: "dev",
};

const started = new Date(Date.now() - 2 * 60 * 1000).toISOString().replace(/\.\d+Z$/, "Z");
const scan = { task_id: "t1", status: "running", started_at: started, finished_at: null, message: "" };
const results = [
  { id: "o1", status: "new", task_id: "t1", created_at: started, intent: "Predict which accounts will churn", solution: "S-1", methodology: "M-1", why: "`accounts.churned` is the label." },
  { id: "o2", status: "not_yet", task_id: "t1", created_at: started, intent: "Forecast revenue", why: "no timestamp column" },
];
let finished = false;
const calls = [];

const answers = () => ({
  viewer,
  workspace_status: { requested: true, ready: true, phase: "Ready", workspace_id: "ws1" },
  list_opportunities: finished
    ? { opportunities: results, last_scan: { ...scan, status: "succeeded", finished_at: new Date().toISOString() } }
    : { opportunities: [], last_scan: scan },
  list_agent_library: { agents: [{ key: "agent-autosuggest", deployed: true }] },
  workspace_query: { methodologies: [] },
});

const failures = [];
const check = (ok, what) => {
  if (!ok) failures.push(what);
  console.log(`${ok ? "ok  " : "FAIL"} ${what}`);
};

const browser = await chromium.launch();
const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
await context.route(/\/v1\/[a-z_]+/, async (route) => {
  const url = new URL(route.request().url());
  const name = url.pathname.split("/v1/")[1];
  calls.push({ name, query: url.search });
  return route.fulfill({ contentType: "application/json", body: JSON.stringify({ ok: true, value: answers()[name] ?? {} }) });
});
const page = await context.newPage();
const errors = [];
page.on("pageerror", (e) => errors.push(String(e)));

await page.goto(`${BASE}/w/ws1/design/opportunities`);
await page.getByText("Scanning… started").first().waitFor({ timeout: 10000 });
check(!page.url().includes("task="), "the page has no ?task= in its address");
check(await page.getByText(/Scanning… started 2m ago/).count() > 0, "the banner says the scan started 2 minutes ago");
const button = page.getByTestId("scan-button");
check((await button.textContent())?.includes("Scanning…") === true, "the Scan button says it is running");
check(await button.isDisabled(), "the Scan button cannot start a second scan while one runs");
check(!calls.some((c) => c.name === "get_agent_task"), "the page follows the scan without get_agent_task");
check(calls.filter((c) => c.name === "list_opportunities").every((c) => !c.query.includes("task_id")), "list_opportunities is read without task_id");
await page.screenshot({ path: `${OUT}/running.png` });

// The scan finishes while the page is open (or while nobody looked): the next read shows it.
finished = true;
await page.getByText("Predict which accounts will churn").first().waitFor({ timeout: 10000 });
check(await page.getByText("Scanning… started").count() === 0, "the running banner is gone once the scan finished");
check(await page.getByText(/Last scanned/).count() > 0, "the banner says when the workspace was last scanned");
check(await page.getByText("Forecast revenue").count() > 0, "the declined Opportunity is under Not Yet");
check(!(await page.getByTestId("scan-button").first().isDisabled()), "Scan again is available");
await page.screenshot({ path: `${OUT}/finished.png`, fullPage: true });
check(errors.length === 0, `no page errors ${errors.join("; ")}`);

await browser.close();
if (failures.length) {
  console.error(`${failures.length} check(s) failed`);
  process.exit(1);
}
