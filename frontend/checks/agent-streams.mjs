// SPDX-License-Identifier: MIT

// Screenshots of the streams that start an agent (lake#677, docs/api/agent-launches.md#stream-triggers)
// at 1440 and 390 wide: an agent's Design page with its streams under Where It Is Used, its Edit
// drawer, and New agent on a frontier model. Run against `npx vite --port 5174`; every /v1 call is
// answered by the mocks below, so no lake is needed. Playwright is not a dependency of the app:
//
//   PLAYWRIGHT_FROM=/path/to/a/project/with/playwright/package.json OUT=/tmp/shots node checks/agent-streams.mjs
//
// It prints a JSON report: per width and page, whether the page scrolls sideways, its headings, the
// stream chips offered, and any page error.
import { createRequire } from "node:module";
import { mkdirSync } from "node:fs";
const require = createRequire(process.env.PLAYWRIGHT_FROM ?? import.meta.url);
const { chromium } = require("playwright");

const OUT = process.env.OUT ?? "/tmp/lake-agent-stream-shots";
mkdirSync(OUT, { recursive: true });
const BASE = process.env.BASE ?? "http://localhost:5174";

const viewer = {
  signed_in: true,
  administrator: false,
  display_name: "Ada Lovelace",
  email: "ada@example.com",
  user_id: "user-1",
  customer_id: "me",
  workspace_id: "ws-1",
  spaces: [{ id: "me", name: "ada-x7f2", label: "Your workspaces", personal: true, kind: "individual", workspaces: [{ id: "ws-1", name: "Support", customer_id: "me", status: "ready" }] }],
  clerk: { enabled: false },
  password_sign_up: false,
  config: { frontier_model: "gpt-4o" },
  version: "dev",
};
const agent = {
  id: "agent-7",
  name: "Order Watcher",
  purpose: "Flag orders that look like fraud as they arrive",
  kind: "custom",
  runtime: "python",
  model_name: "gpt-4o",
  skills: ["search"],
  trigger: "",
  work_area: { kind: "workspace", detail: "" },
  definition: { source: "zetta", instructions: "# Order Watcher", files: [], entrypoint: "" },
  created_at: "2026-10-10T09:00:00Z",
  updated_at: "2026-10-11T09:00:00Z",
};
const trigger = (id, stream) => ({ id, agent_id: "agent-7", agent_name: "Order Watcher", stream, zetta: "workspace", cursor: 41, set_by: "user-1", set_by_name: "Ada Lovelace", created_at: "2026-10-11T10:00:00Z" });
const answers = {
  viewer,
  workspace_status: { requested: true, ready: true, phase: "Ready", workspace_id: "ws-1" },
  list_workspaces: { workspaces: [{ id: "ws-1", name: "Support", environment: "dev", phase: "Ready", desired: "running", storage_gi: 5 }] },
  get_agent: agent,
  list_agents: { agents: [agent] },
  list_agent_library: { agents: [] },
  list_agent_streams: { streams: [trigger("t-1", "orders"), trigger("t-2", "refund-requests-from-the-storefront")] },
  list_agent_schedules: { schedules: [] },
  list_agent_launches: { launches: [{ id: "l-1", agent_id: "agent-7", trigger: "stream", mode: "job", phase: "Succeeded" }] },
  list_agent_revisions: { revisions: [], total: 0 },
  list_models: { models: [] },
  list_projects: { projects: [] },
  list_streams: {
    groups: [
      { zetta: "workspace", topics: [{ topic: "orders" }, { topic: "refund-requests-from-the-storefront" }, { topic: "invoices" }, { topic: "agent-events", system: true }], error: "" },
      { zetta: "bus", topics: [{ topic: "agent-tasks.agent-project", system: true }], error: "" },
    ],
    next_cursor: null,
    previous_cursor: null,
    total: 5,
  },
};

const browser = await chromium.launch();
const report = [];
for (const width of [1440, 390]) {
  const context = await browser.newContext({ viewport: { width, height: width > 600 ? 900 : 844 }, deviceScaleFactor: width > 600 ? 1 : 2, colorScheme: "light" });
  await context.route(/\/v1\/[a-z_]+/, async (route) => {
    const name = new URL(route.request().url()).pathname.split("/v1/")[1];
    return route.fulfill({ contentType: "application/json", body: JSON.stringify({ ok: true, value: answers[name] ?? {} }) });
  });
  const page = await context.newPage();
  const errors = [];
  page.on("pageerror", (e) => errors.push(String(e)));
  const facts = async (tag) => {
    await page.screenshot({ path: `${OUT}/${tag}-${width}.png`, fullPage: true });
    report.push({
      width,
      tag,
      ...(await page.evaluate(() => ({
        sideways: document.documentElement.scrollWidth > window.innerWidth,
        headings: [...document.querySelectorAll("h1, h2")].map((h) => h.textContent?.trim()).filter(Boolean),
        offered: [...document.querySelectorAll('[aria-label="Streams that start it"] button')].map((b) => `${b.textContent?.trim()}${b.getAttribute("aria-pressed") === "true" ? " (on)" : ""}`),
        usedIn: [...document.querySelectorAll("li")].find((li) => li.textContent?.startsWith("Streams that start it"))?.textContent ?? null,
      }))),
      errors: [...errors],
    });
  };
  await page.goto(BASE + "/w/ws-1/design/agent?id=agent-7", { waitUntil: "networkidle" });
  await page.waitForTimeout(400);
  await facts("design");
  await page.getByRole("button", { name: "Edit" }).first().click();
  await page.waitForTimeout(500);
  await page.getByText("Starts on new messages in").scrollIntoViewIfNeeded();
  await facts("edit");
  await page.goto(BASE + "/w/ws-1/design/agents", { waitUntil: "networkidle" });
  await page.waitForTimeout(300);
  await page.getByRole("button", { name: "New agent" }).first().click();
  await page.waitForTimeout(300);
  await page.locator('select[name="model"]').selectOption("frontier");
  await page.getByText("Starts on new messages in").scrollIntoViewIfNeeded();
  await page.waitForTimeout(200);
  await facts("new");
  await context.close();
}
await browser.close();
console.log(JSON.stringify(report, null, 2));
