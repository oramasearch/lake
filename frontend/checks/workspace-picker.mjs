// Screenshots of the top bar's workspace picker (lake#638, docs/ui/workspace-selection.md → The
// picker) at 1440 and 390 wide. Run against `npx vite --port 5174`; every /v1 call is answered by the
// mocks below (one account with three workspaces and two organizations), so no lake is needed.
// Playwright is not a dependency of the app:
//
//   PLAYWRIGHT_FROM=/path/to/a/project/with/playwright/package.json OUT=/tmp/shots node checks/workspace-picker.mjs
//
// It prints a JSON report: per width, whether the page scrolls sideways, the pill's text and
// tooltip, the group headers and whether the filter box shows; a second viewer with twelve
// workspaces shows the filter.
import { createRequire } from "node:module";
import { mkdirSync } from "node:fs";
const require = createRequire(process.env.PLAYWRIGHT_FROM ?? import.meta.url);
const { chromium } = require("playwright");

const OUT = process.env.OUT ?? "/tmp/lake-picker-shots";
mkdirSync(OUT, { recursive: true });
const BASE = process.env.BASE ?? "http://localhost:5174";

const ws = (customer, names) => names.map((name, i) => ({ id: `${customer}-${i}`, name, customer_id: customer, status: "ready" }));
const spaces = (orgA, orgB) => [
  { id: "me", name: "steve-x7f2", label: "Your workspaces", personal: true, kind: "individual", workspaces: ws("me", ["steve", "Churn experiments", "Quarterly revenue forecasts"]) },
  { id: "acme", name: "acme-research", label: "acme-research", personal: false, kind: "organization", workspaces: ws("acme", orgA) },
  { id: "north", name: "northwind-analytics-and-reporting", label: "northwind-analytics-and-reporting", personal: false, kind: "organization", workspaces: ws("north", orgB) },
];
const viewerFor = (s) => ({
  signed_in: true,
  administrator: false,
  display_name: "Steve Sperandeo",
  email: "steve@example.com",
  customer_id: "acme",
  workspace_id: "acme-0",
  spaces: s,
  clerk: { enabled: false },
  password_sign_up: false,
  version: "dev",
});
const VIEWERS = {
  three: viewerFor(spaces(["Production data warehouse", "Staging"], ["Reporting"])),
  many: viewerFor(spaces(["Production data warehouse", "Staging", "Research", "Sandbox", "Pricing"], ["Reporting", "Ops", "Finance", "Marketing"])),
};

const browser = await chromium.launch();
const report = [];
for (const [scenario, viewer] of Object.entries(VIEWERS)) {
  for (const width of [1440, 390]) {
    const context = await browser.newContext({ viewport: { width, height: width > 600 ? 900 : 844 }, deviceScaleFactor: width > 600 ? 1 : 2, colorScheme: "light" });
    await context.route(/\/v1\/[a-z_]+/, async (route) => {
      const name = new URL(route.request().url()).pathname.split("/v1/")[1];
      const value =
        name === "viewer"
          ? viewer
          : name === "workspace_status"
            ? { requested: true, ready: true, phase: "Ready", workspace_id: "acme-0" }
            : name === "list_workspaces"
              ? { workspaces: [{ id: "acme-0", name: "Production data warehouse", environment: "prod", phase: "Ready", desired: "running", storage_gi: 20 }] }
              : {};
      return route.fulfill({ contentType: "application/json", body: JSON.stringify({ ok: true, value }) });
    });
    const page = await context.newPage();
    const errors = [];
    page.on("pageerror", (e) => errors.push(String(e)));
    await page.goto(BASE + "/w/acme-0/design/projects", { waitUntil: "networkidle" });
    await page.waitForTimeout(400);
    const pill = page.locator('header button[aria-haspopup="listbox"]');
    const tag = `${scenario}-${width}`;
    await page.screenshot({ path: `${OUT}/${tag}-closed.png`, clip: { x: 0, y: 0, width, height: 120 } });
    await pill.click();
    await page.waitForTimeout(250);
    await page.screenshot({ path: `${OUT}/${tag}-open.png` });
    const facts = await page.evaluate(() => ({
      sideways: document.documentElement.scrollWidth > window.innerWidth,
      pill: document.querySelector('header button[aria-haspopup="listbox"]')?.textContent,
      tooltip: document.querySelector('header button[aria-haspopup="listbox"]')?.getAttribute("title"),
      groups: [...document.querySelectorAll('[role="listbox"] [role="group"]')].map((g) => g.getAttribute("aria-label")),
      filter: !!document.querySelector('input[aria-label="Filter workspaces"]'),
      newWorkspace: [...document.querySelectorAll("a")].filter((a) => a.textContent?.includes("New workspace")).length,
    }));
    if (facts.filter) {
      await page.keyboard.type("stag");
      await page.waitForTimeout(150);
      await page.screenshot({ path: `${OUT}/${tag}-filtered.png` });
      facts.filtered = await page.evaluate(() => [...document.querySelectorAll('[role="option"]')].map((o) => o.textContent));
    }
    if (scenario === "three") {
      await page.locator("a", { hasText: "New workspace" }).first().click();
      await page.waitForTimeout(400);
      await page.screenshot({ path: `${OUT}/${tag}-new.png` });
      facts.newUrl = page.url().replace(BASE, "");
      facts.owners = await page.evaluate(() => [...document.querySelectorAll('select[name="owner"] option')].map((o) => o.textContent));
    }
    report.push({ scenario, width, ...facts, errors });
    await context.close();
  }
}
// Steve, 2026-10-11: the remembered organization's only workspace was deleted. The app must fall
// back to the person's own first workspace, store it, rewrite the URL, and never say "Choose a
// workspace"; Workspaces lists every owner's workspaces, as the picker does.
{
  const listsByOwner = {
    me: ws("me", ["steve", "Churn experiments", "Quarterly revenue forecasts"]).map((w, i) => ({ ...w, environment: "prod", phase: i === 1 ? "Failed" : "Ready", desired: "running", storage_gi: 5 })),
    acme: [],
    north: ws("north", ["Reporting"]).map((w) => ({ ...w, environment: "dev", phase: "Pending", desired: "running", storage_gi: 5 })),
  };
  const s = spaces([], ["Reporting"]);
  const stale = { ...viewerFor([s[1], s[0], s[2]]), customer_id: "acme", workspace_id: null };
  for (const width of [1440, 390]) {
    const context = await browser.newContext({ viewport: { width, height: width > 600 ? 900 : 844 }, deviceScaleFactor: width > 600 ? 1 : 2, colorScheme: "light" });
    await context.addInitScript(() => localStorage.setItem("lake-workspace", "deleted-ws"));
    await context.route(/\/v1\/[a-z_]+/, async (route) => {
      const url = new URL(route.request().url());
      const name = url.pathname.split("/v1/")[1];
      const value =
        name === "viewer" ? stale
        : name === "workspace_status" ? { requested: true, ready: true, phase: "Ready" }
        : name === "list_workspaces" ? { workspaces: listsByOwner[url.searchParams.get("customer_id")] ?? [] }
        : {};
      return route.fulfill({ contentType: "application/json", body: JSON.stringify({ ok: true, value }) });
    });
    const page = await context.newPage();
    const errors = [];
    page.on("pageerror", (e) => errors.push(String(e)));
    const facts = { scenario: "stale", width };
    await page.goto(BASE + "/w/deleted-ws/design/projects", { waitUntil: "networkidle" });
    await page.waitForTimeout(500);
    facts.linkedUrl = page.url().replace(BASE, "");
    facts.remembered = await page.evaluate(() => localStorage.getItem("lake-workspace"));
    facts.pill = await page.locator('header button[aria-haspopup="listbox"]').textContent();
    await page.evaluate(() => localStorage.setItem("lake-workspace", "deleted-ws"));
    await page.goto(BASE + "/design/projects", { waitUntil: "networkidle" });
    await page.waitForTimeout(500);
    facts.unprefixedUrl = page.url().replace(BASE, "");
    facts.choose = await page.getByText("Choose a workspace", { exact: false }).count();
    await page.goto(BASE + "/workspaces", { waitUntil: "networkidle" });
    await page.waitForTimeout(500);
    await page.screenshot({ path: `${OUT}/workspaces-${width}.png`, fullPage: true });
    facts.groups = await page.evaluate(() => [...document.querySelectorAll("main section[aria-label]")].map((g) => `${g.getAttribute("aria-label")} (${[...g.querySelectorAll("tbody tr")].length})`));
    facts.newRows = await page.locator("main section button", { hasText: "New workspace" }).count();
    report.push({ ...facts, errors });
    await context.close();
  }
}
await browser.close();
console.log(JSON.stringify(report, null, 2));
