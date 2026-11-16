// Screenshots of the delete and revoke confirmations (lake#645, docs/ui/deletion.md) at 1440 and 390
// wide: an organization that still owns a workspace (blocked, with the workspace linked), one that
// owns none (typed handle), a dataset, a secret mount and an Ask Orama conversation with the agent
// tasks it started (lake#673), and your own account (lake#810), blocked by an organization you alone
// own with other members in it, and free to go. Run against `npx vite --port 5174`; every
// /v1 call is answered by the mocks below, so no lake is needed. Playwright is not a dependency:
//
//   PLAYWRIGHT_FROM=/path/to/a/project/with/playwright/package.json OUT=/tmp/shots node checks/deletion.mjs
//
// It prints a JSON report per shot: whether the page scrolls sideways, the dialog's title, its
// lines, and whether the danger button is enabled before and after typing.
import { createRequire } from "node:module";
import { mkdirSync } from "node:fs";
const require = createRequire(process.env.PLAYWRIGHT_FROM ?? import.meta.url);
const { chromium } = require("playwright");

const OUT = process.env.OUT ?? "/tmp/lake-deletion-shots";
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

const orgPlan = (blocked) => ({
  organization_id: "o-1",
  display_name: "Acme Analytics",
  customer_id: "c-1",
  handle: "acme-analytics",
  workspaces: blocked ? [{ id: "w-9", name: "Production data warehouse", environment: "prod" }] : [],
  counts: { teams: 2, memberships: 7, invitations: 1, sso_providers: 1, federation_issuers: 0, secret_mount_routes: 0, policy_objects: 12, grants: 9 },
  dry_run: true,
  deleted: false,
});

const accountPlan = (blocked) => ({
  user_id: "u-1",
  customer_id: "me",
  handle: "ada-x7f2",
  workspaces: [{ id: "me-0", name: "ada", environment: "dev", customer_id: "me" }],
  organizations: [
    { organization_id: "o-2", display_name: "Side project", handle: "side-project", role: "owner", other_owners: 0, other_members: 0, workspaces: [{ id: "w-2", name: "side" }], action: "delete" },
    { organization_id: "o-1", display_name: "Acme Analytics", handle: "acme-analytics", role: "owner", other_owners: blocked ? 0 : 1, other_members: 6, workspaces: [], action: blocked ? "blocked" : "leave" },
  ],
  counts: { sessions: 3, devices: 2, credentials: 2, memberships: 2, grants: 4, billing_contacts: 1 },
  kept: { invoices: 2, open_invoices: 0 },
  dry_run: true,
  deleted: false,
});

function answer(name, body, blocked) {
  if (name === "viewer") return viewer;
  if (name === "workspace_status") return { requested: true, ready: true, phase: "Ready", workspace_id: "me-0" };
  if (name === "get_organization") return { id: "o-1", display_name: "Acme Analytics", customer_id: "c-1", created_at: "2026-09-01T10:00:00Z" };
  if (name === "list_memberships") return { items: [{ id: "m-1", organization_id: "o-1", principal_taap_id: "taap:me", role: "owner" }] };
  if (name === "list_teams" || name === "list_invitations") return { items: [] };
  if (name === "delete_organization") return orgPlan(blocked);
  if (name === "delete_account") return accountPlan(blocked);
  if (name === "list_secret_mounts") return { items: [{ id: "mt-1", name: "payments-vault", workspace_id: "me-0", customer_id: "me", created_at: "2026-09-02T10:00:00Z" }] };
  if (name === "delete_secret_mount")
    return { mount_id: "mt-1", name: "payments-vault", workspace_id: "me-0", dry_run: true, deleted: false, counts: { secrets: 3, versions: 5, encryption_keys: 1, data_keys: 2, leases: 0, mount: 1 }, secrets: ["database", "stripe", "smtp"] };
  if (name === "delete_dataset") return { id: "datasets/orders", dataset: "orders", database: "datasets", dry_run: true, deleted: false, removed: { rows: 120344, size_bytes: 18_400_000, partitions: 1, indexes: ["i-1"] } };
  if (name === "get_ask_conversation")
    return { conversation_id: "cv-1", messages: [{ id: "m-1", role: "user", content: "Which customers are likely to churn next quarter?", created_at: "2026-10-11T10:00:00Z" }, { id: "m-2", role: "assistant", content: "The renewals table shows 42 at risk.", created_at: "2026-10-11T10:00:05Z" }] };
  if (name === "delete_ask_conversation")
    return { conversation_id: "cv-1", messages: 2, first_message: "Which customers are likely to churn next quarter?", agent_tasks: [{ task_id: "t-1", agent: "agent-ask-orama", kind: "ask", status: "succeeded" }, { task_id: "t-2", agent: "agent-autosuggest", kind: "api", status: "succeeded" }], dry_run: true, deleted: false };
  if (name === "workspace_query" && body?.capability === "get_dataset") return { dataset: "orders", rows: 120344, size_bytes: 18400000, format: "parquet", columns: ["id", "total"], partitions: [] };
  return {};
}

const SHOTS = [
  { tag: "org-blocked", path: "/organization?id=o-1", open: "Delete organization", blocked: true },
  { tag: "org", path: "/organization?id=o-1", open: "Delete organization", typed: "acme-analytics" },
  { tag: "mount", path: "/secret-mounts?workspace_id=me-0", open: "Delete", typed: "payments-vault" },
  { tag: "conversation", path: "/?conversation=cv-1", open: "Delete", typed: "delete" },
  { tag: "dataset", path: "/w/me-0/train/dataset?name=orders&database=datasets", open: "Delete dataset", typed: "orders" },
  { tag: "account-blocked", path: "/settings?tab=profile", open: "Delete account", blocked: true },
  { tag: "account", path: "/settings?tab=profile", open: "Delete account", typed: "ada-x7f2" },
];

const browser = await chromium.launch();
const report = [];
for (const shot of SHOTS) {
  for (const width of [1440, 390]) {
    const context = await browser.newContext({ viewport: { width, height: width > 600 ? 900 : 844 }, deviceScaleFactor: width > 600 ? 1 : 2, colorScheme: "light" });
    await context.route(/\/v1\/[a-z_]+/, async (route) => {
      const name = new URL(route.request().url()).pathname.split("/v1/")[1];
      let body = null;
      try {
        body = route.request().postDataJSON();
      } catch {
        body = null;
      }
      return route.fulfill({ contentType: "application/json", body: JSON.stringify({ ok: true, value: answer(name, body, !!shot.blocked) }) });
    });
    const page = await context.newPage();
    const errors = [];
    page.on("pageerror", (e) => errors.push(String(e)));
    await page.goto(BASE + shot.path, { waitUntil: "networkidle" });
    await page.waitForTimeout(400);
    await page.getByRole("button", { name: shot.open, exact: true }).first().click();
    await page.waitForTimeout(500);
    const dialog = page.locator('[role="dialog"]');
    const danger = dialog.locator(".button-danger");
    const before = (await danger.count()) > 0 && (await danger.isEnabled());
    if (shot.typed) {
      try {
        await dialog.locator('input[name="confirm"]').fill(shot.typed, { timeout: 5000 });
      } catch (error) {
        errors.push(`no confirmation field: ${String(error).split("\n")[0]}`);
      }
    }
    await page.waitForTimeout(150);
    const after = (await danger.count()) > 0 && (await danger.isEnabled());
    await page.screenshot({ path: `${OUT}/${shot.tag}-${width}.png` });
    report.push({
      shot: shot.tag,
      width,
      sideways: await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth),
      title: (await dialog.count()) ? await dialog.locator(".dialog-title").textContent() : null,
      body: (await dialog.count()) ? (await dialog.locator(".dialog-body").textContent())?.slice(0, 300) : null,
      lines: await dialog.locator('ul[aria-label="What goes"] li').allTextContents(),
      blocked: await dialog.locator(".notice-warning").count(),
      enabledBefore: before,
      enabledAfter: after,
      errors,
    });
    await context.close();
  }
}
await browser.close();
console.log(JSON.stringify(report, null, 2));
