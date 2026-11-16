// Screenshots of the Usage page's figures (lake#663, docs/ui/usage.md) at 1440 and 390
// wide: Storage is what Lake holds outside the engine, billed; Engine Storage is what the engine
// holds on its own volume, marked "Detail, billed as the engine's volume"; volumes are billed. Run
// against `npx vite --port 5174`; every /v1 call is answered by the mocks below, so no lake is
// needed. Playwright is not a dependency:
//
//   PLAYWRIGHT_FROM=/path/to/a/project/with/playwright/package.json OUT=/tmp/shots node checks/engine-storage.mjs
//
// It prints a JSON report per shot: whether the page scrolls sideways, the text a reader checks
// for, and page errors.
import { createRequire } from "node:module";
import { mkdirSync } from "node:fs";
const require = createRequire(process.env.PLAYWRIGHT_FROM ?? import.meta.url);
const { chromium } = require("playwright");

const OUT = process.env.OUT ?? "/tmp/lake-engine-storage-shots";
mkdirSync(OUT, { recursive: true });
const BASE = process.env.BASE ?? "http://localhost:5174";

const viewer = {
  signed_in: true,
  administrator: false,
  display_name: "Ada Lovelace",
  email: "ada@example.com",
  taap_id: "taap:me",
  customer_id: "me",
  workspace_id: "me-0",
  spaces: [{ id: "me", name: "ada-x7f2", label: "Your workspaces", personal: true, kind: "individual", workspaces: [{ id: "me-0", name: "ada", customer_id: "me", status: "ready" }] }],
  clerk: { enabled: false },
  password_sign_up: false,
  version: "dev",
};

const GIB = 1024 ** 3;
function dimension(name, title, unit, aggregation, total, billed, kinds) {
  return {
    dimension: name,
    title,
    unit,
    aggregation,
    total,
    points: [total],
    by_workspace: [{ key: "me-0", name: "ada", workspace_id: "me-0", total, points: [total] }],
    by_kind: kinds.map(([key, value]) => ({ key, total: value, points: [value] })),
    billed,
  };
}

function usage(input) {
  const from = input?.from ?? "2026-10-01T00:00:00Z";
  return {
    customer_id: "me",
    from,
    to: input?.to ?? "2026-11-01T00:00:00Z",
    granularity: input?.granularity ?? "month",
    periods: [from],
    dimensions: [
      dimension("storage", "Storage", "bytes", "last", 3.5 * GIB, true, [["repositories", 2 * GIB], ["drive_files", 1.5 * GIB]]),
      dimension("engine_storage", "Engine Storage", "bytes", "last", 41 * GIB, false, [["workspace_data", 30 * GIB], ["models", 8 * GIB], ["indexes", 3 * GIB]]),
      dimension("volumes", "Service and Store Volumes", "gib_seconds", "sum", 50 * 2628000, true, [["service", 50 * 2628000]]),
      dimension("cpu", "CPU", "mcpu_seconds", "sum", 1000 * 3600 * 200, true, [["workspace", 1000 * 3600 * 200]]),
      dimension("instances", "Service and App Instances", "instance_seconds", "sum", 3600 * 700, false, [["zetta", 3600 * 700]]),
    ],
    workspaces: [{ workspace_id: "me-0", name: "ada", measured: true, message: "" }],
    not_measured: [],
  };
}

function answer(name, body) {
  if (name === "viewer") return viewer;
  if (name === "workspace_status") return { requested: true, ready: true, phase: "Ready", workspace_id: "me-0" };
  if (name === "usage_over_time") return usage(body);
  if (name === "usage_limits") return { customer_id: "me", month: "2026-10-01T00:00:00Z", limits: [], warnings: [], not_measured: [] };
  if (name === "list_workspaces") return { items: [{ id: "me-0", name: "ada", customer_id: "me" }] };
  if (name === "list_sessions") return { items: [] };
  return {};
}

const SHOTS = [
  { tag: "usage", path: "/usage", expect: ["Engine Storage", "Detail, billed as the engine's volume", "Storage", "Detail, not billed"] },
  { tag: "engine-storage", path: "/usage?dimension=engine_storage", expect: ["Engine Storage", "Workspace data", "Models"] },
];

const browser = await chromium.launch();
const report = [];
for (const shot of SHOTS) {
  for (const width of [1440, 390]) {
    const context = await browser.newContext({ viewport: { width, height: width > 600 ? 900 : 844 }, deviceScaleFactor: width > 600 ? 1 : 2, colorScheme: "light" });
    await context.route(/\/v1\/[a-z_]+/, async (route) => {
      const url = new URL(route.request().url());
      const name = url.pathname.split("/v1/")[1];
      let body = null;
      try {
        body = route.request().postDataJSON();
      } catch {
        body = null;
      }
      return route.fulfill({ contentType: "application/json", body: JSON.stringify({ ok: true, value: answer(name, body) }) });
    });
    const page = await context.newPage();
    const errors = [];
    page.on("pageerror", (e) => errors.push(String(e)));
    page.on("console", (m) => {
      if (m.type() === "error" && !m.text().startsWith("Failed to load resource")) errors.push(m.text().split("\n")[0].slice(0, 300));
    });
    await page.goto(BASE + shot.path, { waitUntil: "networkidle" });
    await page.waitForTimeout(500);
    const text = await page.evaluate(() => document.body.innerText);
    const sideways = await page.evaluate(() => document.documentElement.scrollWidth > document.documentElement.clientWidth);
    const file = `${OUT}/${shot.tag}-${width}.png`;
    await page.screenshot({ path: file, fullPage: true });
    report.push({ shot: shot.tag, width, file, sideways, missing: shot.expect.filter((t) => !text.includes(t)), errors });
    await context.close();
  }
}
await browser.close();
console.log(JSON.stringify(report, null, 2));
