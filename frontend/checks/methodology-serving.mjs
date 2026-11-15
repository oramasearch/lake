// Screenshots of Ship → Model for a model a workspace methodology trained (lake#690,
// docs/ui/ship.md) at 1440 and 390 wide: published and starting, then serving with Try It's
// predictions. Run against `npx vite --port 5175`; every /v1 call is answered by the mocks below,
// so no lake is needed. Playwright is not a dependency:
//
//   PLAYWRIGHT_FROM=/path/to/a/project/with/playwright/package.json OUT=/tmp/shots node checks/methodology-serving.mjs
//
// It prints a JSON report per shot: whether the page scrolls sideways, the text a reader checks
// for, and page errors.
import { createRequire } from "node:module";
import { mkdirSync } from "node:fs";
const require = createRequire(process.env.PLAYWRIGHT_FROM ?? import.meta.url);
const { chromium } = require("playwright");

const OUT = process.env.OUT ?? "/tmp/lake-methodology-serving-shots";
mkdirSync(OUT, { recursive: true });
const BASE = process.env.BASE ?? "http://localhost:5175";

const viewer = {
  signed_in: true, administrator: false, display_name: "Ada Lovelace", email: "ada@example.com", taap_id: "taap:me",
  customer_id: "me", workspace_id: "me-0",
  spaces: [{ id: "me", name: "ada-x7f2", label: "Your workspaces", personal: true, kind: "individual", workspaces: [{ id: "me-0", name: "ada", customer_id: "me", status: "ready" }] }],
  clerk: { enabled: false }, password_sign_up: false, version: "dev",
};
const model = {
  id: "mw-1", name: "Late payers", kind: "python:W-0a1b2c3d", project: "", project_name: "", pipeline_name: "",
  run: "run-1", run_id: "run-1", metrics: { held_out_auc: 0.86, in_sample_auc: 0.9 },
  feature_schema: { methodology: "W-0a1b2c3d", lake_run_id: "lr-1", features: ["days_late", "amount"], artifact_format: "pickle" },
  exposed: true, created: "2026-10-11T09:00:00Z", has_weights: true,
};

function answer(name, body, url, phase) {
  if (name === "viewer") return viewer;
  if (name === "workspace_status") return { requested: true, ready: true, phase: "Ready", workspace_id: "me-0" };
  if (name === "workspace_query") {
    const capability = body?.capability ?? url.searchParams.get("capability");
    if (capability === "list_models") return { models: [model] };
    if (capability === "list_studies") return { studies: [] };
    return {};
  }
  if (name === "get_methodology_deployment")
    return { deployment: { id: "d-1", model_id: "mw-1", methodology_id: "W-0a1b2c3d", phase, ready: phase === "Ready",
      message: phase === "Ready" ? "Serving predictions." : "Starting the server: checking its sandbox and loading the model." } };
  if (name === "predict")
    return { model: "mw-1", runner: "methodology", methodology_id: "W-0a1b2c3d", entity_column: "entity",
      predictions: [{ row: 0, entity: "c-1", prediction: 0.82 }, { row: 1, entity: "c-2", prediction: 0.14 }] };
  if (name === "model_traffic") return { models: [] };
  if (name === "list_sessions") return { items: [] };
  if (name === "usage_over_time") return { dimensions: [], buckets: [] };
  return {};
}

const SHOTS = [
  { tag: "starting", phase: "Starting", expect: ["Served by its own code", "W-0a1b2c3d", "starting", "answers once it is serving"] },
  { tag: "serving", phase: "Ready", predict: true, expect: ["Served by its own code", "serving", "Prediction", "c-1", "0.820"] },
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
      try { body = route.request().postDataJSON(); } catch { body = null; }
      return route.fulfill({ contentType: "application/json", body: JSON.stringify({ ok: true, value: answer(name, body, url, shot.phase) }) });
    });
    const page = await context.newPage();
    const errors = [];
    page.on("pageerror", (e) => errors.push(String(e)));
    page.on("console", (m) => { if (m.type() === "error" && !m.text().startsWith("Failed to load resource")) errors.push(m.text().split("\n")[0].slice(0, 300)); });
    await page.goto(BASE + "/w/me-0/ship/model?id=mw-1", { waitUntil: "networkidle" });
    await page.waitForTimeout(500);
    if (shot.predict) {
      try { await page.getByRole("button", { name: "Predict" }).first().click({ timeout: 5000 }); } catch (e) { errors.push(`no Predict: ${String(e).split("\n")[0]}`); }
      await page.waitForTimeout(500);
    }
    const text = await page.locator("body").innerText();
    await page.screenshot({ path: `${OUT}/${shot.tag}-${width}.png`, fullPage: true });
    report.push({ shot: shot.tag, width, sideways: await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth),
      missing: shot.expect.filter((words) => !text.toLowerCase().includes(words.toLowerCase())), errors });
    await context.close();
  }
}
await browser.close();
console.log(JSON.stringify({ report }, null, 2));
