// New Methodology's Ask Orama (lake#688) in the real app, at 1440 and 390 px: Design → Methodologies,
// New methodology, a name and a description, Ask Orama; the drawer while Ask Orama writes, then the
// draft it wrote (checked by Lake), then a draft Lake refused (its template, with the failing
// checks), then a Lake where Ask Orama is not running. Run against `npx vite --port 5174`; every /v1
// call is answered by the mocks below, so no lake is needed. Playwright is not a dependency:
//
//   PLAYWRIGHT_FROM=/path/to/a/project/with/playwright/package.json OUT=/tmp/draft node checks/methodology-draft.mjs
//
// It writes <case>-<width>.png to OUT and prints a JSON report: per case, the turn sent, the badge
// shown, whether the files are listed, any page error, and the document's width against the viewport.
import { createRequire } from "node:module";
import { mkdirSync } from "node:fs";
const require = createRequire(process.env.PLAYWRIGHT_FROM ?? import.meta.url);
const { chromium } = require("playwright");

const OUT = process.env.OUT ?? "/tmp/lake-methodology-draft";
mkdirSync(OUT, { recursive: true });
const BASE = process.env.BASE ?? "http://localhost:5174";

const viewer = {
  signed_in: true,
  administrator: false,
  display_name: "Ada Lovelace",
  email: "ada@example.com",
  customer_id: "c1",
  workspace_id: "ws1",
  spaces: [{ id: "c1", name: "ada", label: "Ada", personal: true, workspaces: [{ id: "ws1", name: "Main", customer_id: "c1", status: "ready" }] }],
  clerk: { enabled: false },
  password_sign_up: false,
  version: "dev",
};
const methodologies = [
  { id: "M-1", name: "Tabular supervised learning", family: "The tabular decision core", source: "orama", runnable: true, inputs: [], outputs: [], metrics: [], model_kinds: [] },
];
const manifest = {
  interface: 1, name: "Late payers", description: "Which customers will pay their invoice late.", entrypoint: "methodology.py",
  inputs: { kinds: ["tabular"], target: { required: true, kind: "binary" }, required_features: [] },
  parameters: [{ name: "learning_rate", kind: "float", default: 0.1, minimum: 0.001, maximum: 1, log: true }],
  metrics: ["auc", "log_loss"], explain: false, packages: "requirements.lock",
  resources: { cpu: "1", memory: "2Gi", gpu: 0, timeout_seconds: 3600 },
};
const files = [
  { path: "methodology.json", contents: JSON.stringify(manifest, null, 2) + "\n" },
  { path: "methodology.py", contents: "import math\nimport pickle\n\n\ndef fit(data, params):\n    \"\"\"A logistic regression by gradient descent on the numeric features.\"\"\"\n    return pickle.dumps({\"weights\": [0.0] * len(data.features)})\n\n\ndef predict(artifact, data):\n    return [0.5 for _ in data.rows]\n\n\ndef score(artifact, data):\n    return {\"auc\": 0.5, \"log_loss\": math.log(2)}\n" },
  { path: "requirements.lock", contents: "numpy==2.1.3 \\\n    --hash=sha256:" + "a".repeat(64) + "\n" },
];
const drafts = {
  orama: { name: "Late payers", drafted_by: "orama", task_id: "t-draft", reason: "A logistic regression fits a yes-or-no outcome such as paying late.", manifest, files, findings: [{ check: "manifest", ok: true, message: "reads" }] },
  refused: {
    name: "Late payers", drafted_by: "template", template: "binary", reason: "A yes-or-no outcome: logistic regression on the numeric features.", manifest, files: files.map((f) => (f.path === "requirements.lock" ? { ...f, contents: "" } : f)),
    orama: { task_id: "t-draft", offered: false, reason: "Ask Orama's draft did not pass the checks Validate runs first, so this is Lake's template instead.", findings: [{ check: "packages", ok: false, message: "requirements.lock line 1: numpy==2.1.3 needs --hash=sha256:<64 lowercase hex> for each file it may install" }, { check: "entrypoint.score", ok: false, message: "methodology.py does not define score(artifact, data) at module level" }] },
  },
  template: { name: "Late payers", drafted_by: "template", template: "binary", reason: "A yes-or-no outcome: logistic regression on the numeric features.", manifest, files: files.map((f) => (f.path === "requirements.lock" ? { ...f, contents: "" } : f)) },
};

const cases = [
  ["writing", true, "orama", true],
  ["orama", true, "orama", false],
  ["refused", true, "refused", false],
  ["no-agent", false, "template", false],
];

const browser = await chromium.launch();
const report = [];
for (const width of [1440, 390]) {
  for (const [name, deployed, draft, hold] of cases) {
    const context = await browser.newContext({ viewport: { width, height: width > 500 ? 900 : 844 }, deviceScaleFactor: width > 500 ? 1 : 2 });
    const sent = [];
    await context.route(/\/v1\/[a-z_]+/, async (route) => {
      const capability = new URL(route.request().url()).pathname.split("/v1/")[1];
      const body = route.request().postData();
      let value = {};
      if (capability === "viewer") value = viewer;
      else if (capability === "workspace_status") value = { requested: true, ready: true, phase: "Ready", workspace_id: "ws1" };
      else if (capability === "workspace_query" || capability === "list_methodologies") value = { methodologies };
      else if (capability === "list_agent_library") value = { agents: deployed ? [{ key: "agent-ask-orama", name: "Ask Orama", deployed: true }] : [] };
      else if (capability === "agent_turn") {
        sent.push(JSON.parse(body || "{}"));
        value = { task_id: "t-draft", agent: "agent-ask-orama", status: "pending" };
      } else if (capability === "get_agent_task") {
        value = hold
          ? { task_id: "t-draft", agent: "agent-ask-orama", kind: "methodology_draft", status: "running", message: "Working on it" }
          : { task_id: "t-draft", agent: "agent-ask-orama", kind: "methodology_draft", status: "succeeded", answer: { ok: true, reply: "I wrote a first version." } };
      } else if (capability === "draft_methodology") {
        sent.push({ draft_methodology: JSON.parse(body || "{}") });
        value = { draft: drafts[draft] };
      }
      return route.fulfill({ contentType: "application/json", body: JSON.stringify({ ok: true, value }) });
    });
    const page = await context.newPage();
    const errors = [];
    page.on("pageerror", (e) => errors.push(String(e)));
    await page.goto(`${BASE}/w/ws1/design/methodologies`);
    await page.getByRole("button", { name: "New methodology" }).first().click();
    await page.locator('input[name="name"]').fill("Late payers");
    await page.locator('textarea[name="description"]').fill("Predict which customers will pay their invoice late, from their history.");
    await page.locator('select[name="target_kind"]').selectOption({ index: 1 });
    await page.getByRole("dialog").getByRole("button", { name: "Ask Orama" }).click();
    await page.waitForTimeout(hold ? 1500 : 3500);
    const facts = await page.evaluate(() => {
      const drawer = document.querySelector('[role="dialog"]') ?? document.body;
      return {
        text: drawer.innerText.slice(0, 2000),
        files: [...drawer.querySelectorAll("details summary")].map((s) => s.textContent),
        scrollWidth: document.documentElement.scrollWidth,
      };
    });
    await page.screenshot({ path: `${OUT}/${name}-${width}.png` });
    report.push({
      case: name,
      width,
      turn: sent.find((s) => s.agent) ?? null,
      draft_call: sent.find((s) => s.draft_methodology)?.draft_methodology ?? null,
      badge: (facts.text.match(/Written by Ask Orama|Lake's template/) ?? [""])[0],
      writing: /Ask Orama is writing a first version/.test(facts.text),
      not_running: /not running on this Lake/.test(facts.text),
      failing_listed: /entrypoint\.score/.test(facts.text),
      files: facts.files,
      overflow: facts.scrollWidth > width,
      errors,
    });
    await context.close();
  }
}
await browser.close();
console.log(JSON.stringify(report, null, 1));
