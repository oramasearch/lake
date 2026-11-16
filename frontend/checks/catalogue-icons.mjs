// The catalogue icons (lake#639) in the real app, light and dark, at 1440 and 390 px: Design →
// Solutions, Methodologies, a solution, a methodology, Opportunities, the Catalogue Icons gallery and
// Search's Solutions place. Run against `npx vite --port 5174`; every /v1 call is answered by the
// mocks below, so no lake is needed. Playwright is not a dependency of the app:
//
//   PLAYWRIGHT_FROM=/path/to/a/project/with/playwright/package.json OUT=/tmp/icons node checks/catalogue-icons.mjs
//
// It writes <page>-<width>-<theme>.png to OUT and prints a JSON report: per page, how many icons
// drew, any without a label, any page error, and the document's width against the viewport.
import { createRequire } from "node:module";
import { mkdirSync } from "node:fs";
const require = createRequire(process.env.PLAYWRIGHT_FROM ?? import.meta.url);
const { chromium } = require("playwright");

const OUT = process.env.OUT ?? "/tmp/lake-icon-shots";
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

const M = (id, name, family, runnable = false) => ({ id, name, family, runnable, inputs: ["a labelled table"], outputs: ["scores"], metrics: ["AUC"], model_kinds: ["gbdt"] });
const methodologies = [
  M("M-1", "Tabular supervised learning", "The tabular decision core", true),
  M("M-5", "Survival / time-to-event", "The tabular decision core"),
  M("M-17", "Bayesian / probabilistic modeling & uncertainty quantification", "The tabular decision core"),
  M("M-18", "Ensembling & stacking", "The tabular decision core"),
  M("M-2", "Entity resolution", "Identity & structure", true),
  M("M-3", "Segmentation / clustering", "Identity & structure", true),
  M("M-12", "Graph ML", "Identity & structure"),
  M("M-4", "Time-series forecasting", "Time & vigilance", true),
  M("M-7", "Anomaly detection", "Time & vigilance"),
  M("M-21", "Online / streaming learning", "Time & vigilance"),
  M("M-6", "Causal inference & uplift", "Action & decision"),
  M("M-8", "Recommendation / next-best-action", "Action & decision"),
  M("M-9", "Learning-to-rank", "Action & decision"),
  M("M-15", "Bandits & sequential decisioning", "Action & decision"),
  M("M-16", "Optimization / operations research", "Action & decision"),
  M("M-10", "NLP / embeddings / RAG / fine-tuning", "Language, documents & perception"),
  M("M-11", "Document intelligence", "Language, documents & perception"),
  M("M-13", "Computer vision", "Language, documents & perception"),
  M("M-14", "Speech & audio analytics", "Language, documents & perception"),
  M("M-19", "Label efficiency", "The data supply chain"),
  M("M-20", "Privacy-preserving learning & synthetic data", "The data supply chain"),
  M("M-30", "A methodology the set has not drawn", "Time & vigilance"),
];
const S = (id, name, primary, status = "catalogue", runnable = false) => ({
  id,
  name,
  source: "orama",
  status,
  runnable,
  promise: `What ${name} does for the business, in one sentence.`,
  methodologies: [{ id: primary, role: "primary", minimum_level: 1 }, { id: "M-2", role: "foundation", minimum_level: 1 }],
});
const solutions = [
  S("S-1", "Churn & Retention", "M-1", "catalogue", true),
  S("S-2", "Lead & Opportunity Scoring", "M-1"),
  S("S-3", "Customer Lifetime Value", "M-1"),
  S("S-4", "Collections Prioritization", "M-1"),
  S("S-5", "Demand Forecasting", "M-4", "catalogue", true),
  S("S-6", "Replenishment & Inventory Action", "M-4"),
  S("S-7", "KPI Anomaly & Driver Analysis", "M-7"),
  S("S-8", "Predictive Maintenance", "M-5"),
  S("S-9", "Automated Outbound", "M-1"),
  S("S-10", "Fraud & Financial-Crime Decisioning", "M-1"),
  S("S-11", "Next-Best-Action", "M-8"),
  S("S-12", "Claims Triage & Fraud Referral", "M-1"),
  S("B-1", "Dynamic Pricing & Discount Governance", "M-4", "sketch"),
  S("B-2", "Employee Attrition (aggregate-first)", "M-1", "sketch"),
  S("B-3", "Ticket Triage & Service Automation", "M-10", "sketch"),
  S("B-4", "Document & Workflow Automation", "M-11", "sketch"),
  S("B-5", "Supplier Risk & Procurement Intelligence", "M-1", "sketch"),
  S("B-6", "Healthcare Operations (no-show & denials)", "M-1", "sketch"),
  S("B-7", "Credit Underwriting & Limit Management", "M-1", "sketch"),
  S("B-8", "IT & Security Operations", "M-12", "sketch"),
  { id: "w1", source: "workspace", editable: true, name: "Our churn, tuned", decision: "Who the success team calls this week.", duplicated_from: "S-1", methodologies: [{ id: "M-1", minimum_level: 1 }] },
  { id: "w2", source: "workspace", editable: true, name: "Store clusters", decision: "Which stores share a promotion plan.", methodologies: [{ id: "M-3", minimum_level: 1 }] },
  { id: "w3", source: "workspace", editable: true, name: "Shift alerts", decision: "When to call in a second shift.", methodologies: [{ id: "M-30", minimum_level: 1 }] },
  { id: "w4", source: "workspace", editable: true, name: "Vendor shortlist", decision: "Which vendors to invite to tender.", methodologies: [] },
];
const opportunities = [
  { id: "o1", status: "new", task_id: "t1", created_at: "2026-10-11T10:00:00Z", intent: "Find customers likely to cancel next quarter", solution: "S-1", methodology: "M-1", why: "The `subscriptions` table has cancellations." },
  { id: "o2", status: "new", task_id: "t1", created_at: "2026-10-11T10:00:00Z", intent: "Forecast weekly demand per store", solution: "S-5", methodology: "M-4", why: "The `sales` table has two years of weekly sales." },
  { id: "o3", status: "new", task_id: "t1", created_at: "2026-10-11T10:00:00Z", intent: "Group stores by their buying patterns", methodology: "M-3", why: "The `stores` table." },
];

function engine(body) {
  const capability = JSON.parse(body || "{}").capability;
  if (capability === "list_methodologies") return { methodologies };
  if (capability === "list_solutions") return { solutions: solutions.filter((s) => s.source === "orama") };
  return {};
}

const answers = {
  viewer,
  workspace_status: { requested: true, ready: true, phase: "Ready", workspace_id: "ws1" },
  list_solutions: { solutions, catalogue: { available: true } },
  list_opportunities: { opportunities },
  list_agent_library: { agents: [] },
  list_runs: { runs: [] },
  list_repositories: { items: [] },
  list_docs: { pages: [] },
  search_drives: { items: [] },
  list_transcripts: { transcripts: [] },
};

const pages = [
  ["solutions", "/w/ws1/design/solutions"],
  ["methodologies", "/w/ws1/design/methodologies"],
  ["solution", "/w/ws1/design/solution?id=S-1"],
  ["own-solution", "/w/ws1/design/solution?id=w3"],
  ["methodology", "/w/ws1/design/methodology?id=M-4"],
  ["opportunities", "/w/ws1/design/opportunities"],
  ["gallery", "/design/icons"],
  ["search", "/search?q=st&in=solutions"],
];

const browser = await chromium.launch();
const report = [];
for (const width of [1440, 390]) {
  for (const theme of ["light", "dark"]) {
    const context = await browser.newContext({ viewport: { width, height: width > 500 ? 900 : 844 }, deviceScaleFactor: width > 500 ? 1 : 2 });
    await context.addInitScript((t) => localStorage.setItem("lake-theme", t), theme);
    await context.route(/\/v1\/[a-z_]+/, async (route) => {
      const name = new URL(route.request().url()).pathname.split("/v1/")[1];
      let value = answers[name] ?? {};
      if (name === "workspace_query") value = engine(route.request().postData());
      if (name === "get_solution") {
        const id = JSON.parse(route.request().postData() || "{}").id ?? new URL(route.request().url()).searchParams.get("id");
        value = { solution: solutions.find((s) => s.id === id) ?? null };
      }
      return route.fulfill({ contentType: "application/json", body: JSON.stringify({ ok: true, value }) });
    });
    const page = await context.newPage();
    const errors = [];
    page.on("pageerror", (e) => errors.push(String(e)));
    for (const [name, path] of pages) {
      await page.goto(`${BASE}${path}`);
      await page.waitForTimeout(1200);
      const facts = await page.evaluate(() => {
        const icons = [...document.querySelectorAll("main .cat-icon")];
        return {
          icons: icons.length,
          unlabelled: icons.filter((i) => !i.getAttribute("aria-label")).length,
          kinds: [...new Set(icons.map((i) => i.dataset.icon))].length,
          scrollWidth: document.documentElement.scrollWidth,
        };
      });
      await page.screenshot({ path: `${OUT}/${name}-${width}-${theme}.png`, fullPage: name === "gallery" || name === "solutions" });
      report.push({ page: name, width, theme, ...facts, overflow: facts.scrollWidth > width, errors: errors.splice(0) });
    }
    await context.close();
  }
}
await browser.close();
console.log(JSON.stringify(report, null, 1));
