// SPDX-License-Identifier: MIT

// Screenshots of a standalone model (lake#654, docs/ui/train.md#a-standalone-model) at 1440 and 390
// wide: Train → Models with New model, the New Model choice, Train one and Bring one, a trained
// standalone model's page (what it trains on, Retrain, New version, Move into a project, Delete), a
// brought one's page, and Ship → Model serving a standalone model. Run against
// `npx vite --port 5174`; every /v1 call is answered by the mocks below, so no lake is needed.
// Playwright is not a dependency:
//
//   PLAYWRIGHT_FROM=/path/to/a/project/with/playwright/package.json OUT=/tmp/shots node checks/standalone-models.mjs
//
// It prints a JSON report per shot: whether the page scrolls sideways, the dialog's title, the
// text a reader checks for, page errors, and what Train one sent to `train_model`. lake#681 adds
// Bring one from this computer with an ONNX file and its columns (what `register_model` was
// sent), and the Weights card's downloads (the file `export_model`'s answer saved as).
import { createRequire } from "node:module";
import { mkdirSync } from "node:fs";
const require = createRequire(process.env.PLAYWRIGHT_FROM ?? import.meta.url);
const { chromium } = require("playwright");

const OUT = process.env.OUT ?? "/tmp/lake-standalone-model-shots";
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

const version = (id, created, auc, extra = {}) => ({
  id,
  name: "churn",
  kind: "classifier",
  project: "p-internal",
  project_name: "churn (standalone model)",
  pipeline_name: "churn",
  run: `run-${id}`,
  run_id: `run-${id}`,
  metrics: { held_out_auc: auc, in_sample_auc: auc + 0.05, held_out_rows: 100 },
  feature_schema: { features: ["support_tickets", "tenure_months", "monthly_spend", "failed_payments"] },
  exposed: false,
  created,
  has_weights: true,
  standalone: true,
  ...extra,
});

const models = [
  version("m-1", "2026-10-09T10:00:00Z", 0.81),
  version("m-2", "2026-10-11T09:00:00Z", 0.84, { exposed: true }),
  {
    id: "b-1",
    name: "partner churn",
    kind: "classifier",
    project: "p-brought",
    project_name: "partner churn (standalone model)",
    pipeline_name: "",
    run: "",
    run_id: "",
    metrics: { held_out_auc: 0.79 },
    feature_schema: { features: ["tenure_months", "failed_payments"], brought: true, source: "drive Models: partner-churn.json", objective: "binary_logistic", trees: 40 },
    exposed: false,
    created: "2026-10-10T12:00:00Z",
    has_weights: true,
    standalone: true,
  },
  {
    id: "p-1",
    name: "upsell",
    kind: "classifier",
    project: "p-2",
    project_name: "Retention",
    pipeline_name: "weekly upsell",
    run: "run-p-1",
    run_id: "run-p-1",
    metrics: { held_out_auc: 0.7 },
    feature_schema: { features: ["seats"] },
    exposed: false,
    created: "2026-10-08T10:00:00Z",
    has_weights: true,
    standalone: false,
  },
];

const graph = {
  nodes: [
    { id: "load", kind: "connect", name: "Load accounts", attributes: { source: "datasets", table: "accounts" } },
    { id: "fit", kind: "model", name: "churn", attributes: { standalone_training: { model_name: "churn", methodology: "M-1", dataset_path: "datasets.accounts", target: "churned", holdout_fraction: 0.2 } } },
  ],
  edges: [],
};

const sent = [];
const registered = [];
const exported = [];
const downloads = [];

function engine(capability, input) {
  if (capability === "list_models") return { models };
  if (capability === "list_studies") return { studies: [] };
  if (capability === "list_datasets") return { datasets: [{ database: "datasets", name: "accounts", columns: ["account_id", "support_tickets", "tenure_months", "monthly_spend", "failed_payments", "churned"] }] };
  if (capability === "list_methodologies")
    return {
      methodologies: [
        { id: "M-1", name: "Binary classification", family: "supervised", runnable: true, inputs: [{ role: "entity", required: true }, { role: "label", required: true }] },
        { id: "M-3", name: "Segmentation", family: "unsupervised", runnable: true, inputs: [{ role: "entity", required: true }, { role: "feature", required: true }] },
        { id: "M-9", name: "Ranking", family: "supervised", runnable: false, inputs: [] },
      ],
    };
  if (capability === "get_graph") return graph;
  if (capability === "get_run_report") return { run: { id: input?.run_id }, model: {}, features: { used: [], excluded: [] }, dataset: { source: "datasets.accounts", accounts: 400, churn_rate: 0.31 } };
  return {};
}

function answer(name, body, url) {
  if (name === "viewer") return viewer;
  if (name === "workspace_status") return { requested: true, ready: true, phase: "Ready", workspace_id: "me-0" };
  if (name === "workspace_query") {
    const capability = body?.capability ?? url.searchParams.get("capability");
    let input = body?.input;
    if (!input) {
      try {
        input = JSON.parse(url.searchParams.get("input") ?? "{}");
      } catch {
        input = {};
      }
    }
    return engine(capability, input);
  }
  if (name === "train_model") {
    sent.push(body);
    return { run_id: "run-new", name: body?.name, project_id: "p-new", pipeline: body?.name, standalone: true };
  }
  if (name === "register_model") {
    registered.push({ name: body?.name, feature_names: body?.feature_names, weights_base64: typeof body?.weights_base64 === "string" ? body.weights_base64.slice(0, 12) : null, weights: body?.weights ?? null, source: body?.source });
    return { model_id: "b-1", name: body?.name, kind: "classifier", project_id: "p-brought", features: body?.feature_names ?? [], standalone: true, format: "onnx" };
  }
  if (name === "export_model") {
    const format = url.searchParams.get("format") ?? "zetta";
    exported.push({ model_id: url.searchParams.get("model_id"), format });
    return format === "onnx"
      ? { model_id: "m-2", name: "churn", format, file_name: "churn-m-2.onnx", media_type: "application/onnx", encoding: "base64", content: "CAgSBXpldHRh", bytes: 9 }
      : { model_id: "m-2", name: "churn", format, file_name: "churn-m-2.json", media_type: "application/json", encoding: "utf-8", content: '{"objective":"binary_logistic","trees":[]}', bytes: 42 };
  }
  if (name === "recent_drive_entries") return { items: [{ drive: { id: "d-1", name: "Models" }, entry: { id: "e-1", kind: "file", name: "partner-churn.json", path: "partner-churn.json" } }] };
  if (name === "list_repositories") return { items: [{ id: "r-1", name: "churn-models", workspace_id: "me-0" }] };
  if (name === "model_traffic") return { models: [] };
  if (name === "list_sessions") return { items: [] };
  if (name === "get_run") return { id: "run-new", status: "running" };
  if (name === "usage_over_time") return { dimensions: [], buckets: [] };
  return {};
}

const ONLY = process.env.ONLY;
const SHOTS = [
  { tag: "models", path: "/w/me-0/train/models", expect: ["New model", "trained on its own", "brought from a file", "Standalone"] },
  { tag: "new-choice", path: "/w/me-0/train/models", click: ["New model"], expect: ["Train one", "Bring one"] },
  { tag: "train-one", path: "/w/me-0/train/models", click: ["New model", "Train one"], fill: true, expect: ["Column to predict", "Held-out share"] },
  { tag: "bring-one", path: "/w/me-0/train/models", click: ["New model", "Bring one", "A drive"], expect: ["What can be brought", "partner-churn.json", "ONNX", "Columns"] },
  { tag: "bring-onnx", path: "/w/me-0/train/models", click: ["New model", "Bring one"], upload: true, expect: ["An ONNX file names no columns", "LightGBM"] },
  { tag: "weights", path: "/w/me-0/train/model?id=m-2", download: ["Download", "As ONNX"], expect: ["v2 of churn as a file", "Download", "As ONNX"] },
  { tag: "model", path: "/w/me-0/train/model?id=m-2", expect: ["No project or pipeline to look after", "datasets.accounts", "churned", "20% of rows", "Retrain", "New version", "Move into a project", "Delete"] },
  { tag: "new-version", path: "/w/me-0/train/model?id=m-2", click: ["New version"], expect: ["New Version of churn", "Train new version"] },
  { tag: "move", path: "/w/me-0/train/model?id=m-2", click: ["Move into a project"], expect: ["Project name"] },
  { tag: "brought", path: "/w/me-0/train/model?id=b-1", expect: ["Brought from a file", "drive Models: partner-churn.json", "New version"] },
  { tag: "ship", path: "/w/me-0/ship/model?id=m-2", expect: ["trained on its own", "predict"] },
];

const browser = await chromium.launch();
const report = [];
for (const shot of SHOTS.filter((s) => !ONLY || s.tag === ONLY)) {
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
      return route.fulfill({ contentType: "application/json", body: JSON.stringify({ ok: true, value: answer(name, body, url) }) });
    });
    const page = await context.newPage();
    const errors = [];
    page.on("pageerror", (e) => errors.push(String(e)));
    // A render error is caught by the page's boundary and logged, not thrown.
    page.on("console", (m) => {
      if (m.type() === "error" && !m.text().startsWith("Failed to load resource")) errors.push(process.env.FULL ? m.text().slice(0, 3000) : m.text().split("\n")[0].slice(0, 300));
    });
    await page.goto(BASE + shot.path, { waitUntil: "networkidle" });
    await page.waitForTimeout(500);
    for (const label of shot.click ?? []) {
      try {
        await page.getByRole("button", { name: label }).first().click({ timeout: 5000 });
      } catch (error) {
        errors.push(`no button ${label}: ${String(error).split("\n")[0]}`);
      }
      await page.waitForTimeout(400);
    }
    if (shot.upload) {
      const dialog = page.locator('[role="dialog"]');
      await dialog.locator("input").first().fill("lightgbm churn");
      await dialog.locator('input[type="file"]').setInputFiles({ name: "churn.onnx", mimeType: "application/octet-stream", buffer: Buffer.from([8, 8, 18, 5, 122, 101, 116, 116, 97]) });
      await dialog.getByPlaceholder("tenure_months, monthly_spend, support_tickets").fill("tenure_months, monthly_spend, support_tickets, failed_payments");
      await page.waitForTimeout(200);
    }
    if (shot.fill) {
      const dialog = page.locator('[role="dialog"]');
      await dialog.locator("input").first().fill("churn v2");
      const selects = dialog.locator("select");
      await selects.nth(0).selectOption("datasets.accounts");
      await page.waitForTimeout(200);
      await selects.nth(2).selectOption("churned");
      await page.waitForTimeout(200);
    }
    const text = await page.locator("body").innerText();
    await page.screenshot({ path: `${OUT}/${shot.tag}-${width}.png` });
    if (shot.upload && width === 1440) {
      await page.locator('[role="dialog"]').getByRole("button", { name: "Bring model" }).click();
      await page.waitForTimeout(600);
    }
    if (shot.download && width === 1440) {
      for (const label of shot.download) {
        const [download] = await Promise.all([page.waitForEvent("download", { timeout: 5000 }), page.getByRole("button", { name: label, exact: true }).click()]);
        downloads.push({ button: label, file: download.suggestedFilename() });
        await page.waitForTimeout(300);
      }
    }
    if (shot.fill && width === 1440) {
      await page.locator('[role="dialog"]').getByRole("button", { name: "Train", exact: true }).click();
      await page.waitForTimeout(500);
    }
    const dialog = page.locator('[role="dialog"]');
    report.push({
      shot: shot.tag,
      width,
      sideways: await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth),
      title: (await dialog.count()) ? await dialog.locator(".dialog-title").first().textContent() : null,
      missing: (shot.expect ?? []).filter((words) => !text.includes(words)),
      errors,
    });
    await context.close();
  }
}
await browser.close();
console.log(JSON.stringify({ report, train_model: sent, register_model: registered, export_model: exported, downloads }, null, 2));
