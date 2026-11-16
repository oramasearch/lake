// SPDX-License-Identifier: MIT

// Screenshots of which services and apps mount a store, and its used space (lake#666,
// docs/ui/drives.md#storage) at 1440 and 390 wide: Data → Drives → Storage with Mounted By and used
// space, the delete dialog of a mounted volume, Ship → Services with a service's storage and its
// Storage dialog, and an app's Services tab choosing storage. Run against `npx vite --port 5175`;
// every /v1 call is answered by the mocks below, so no lake is needed. Playwright is not a
// dependency:
//
//   PLAYWRIGHT_FROM=/path/to/a/project/with/playwright/package.json OUT=/tmp/shots node checks/store-mounts.mjs
//
// It prints a JSON report per shot: whether the page scrolls sideways, the text of what it checks,
// and any page errors.
import { createRequire } from "node:module";
import { mkdirSync } from "node:fs";
const require = createRequire(process.env.PLAYWRIGHT_FROM ?? import.meta.url);
const { chromium } = require("playwright");

const OUT = process.env.OUT ?? "/tmp/lake-store-mounts-shots";
mkdirSync(OUT, { recursive: true });
const BASE = process.env.BASE ?? "http://localhost:5175";

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

const stores = [
  {
    id: "st-1",
    customer_id: "me",
    workspace_id: "me-0",
    type: "volume",
    provider: "cluster",
    name: "uploads",
    slug: "st-111111111111",
    size_gi: 10,
    desired: "present",
    phase: "Ready",
    message: "claim bound",
    endpoint: "",
    connected: true,
    used_bytes: 2_684_354_560,
    used_bytes_at: "2026-10-11T18:00:00Z",
    mounted_by: [{ kind: "service", id: "svc-1", name: "orders-db", mount_path: "/var/lib/data", read_only: false }],
  },
  {
    id: "st-2",
    customer_id: "me",
    workspace_id: "me-0",
    type: "object",
    provider: "s3",
    name: "media",
    slug: "st-222222222222",
    size_gi: 10,
    desired: "present",
    phase: "Ready",
    message: "credentials delivered",
    endpoint: "https://s3.us-east-1.amazonaws.com/acme-media",
    connected: true,
    used_bytes: null,
    used_bytes_at: null,
    mounted_by: [
      { kind: "app", id: "app-1", name: "storefront", mount_path: "", read_only: true },
      { kind: "service", id: "svc-1", name: "orders-db", mount_path: "", read_only: false },
    ],
  },
  {
    id: "st-3",
    customer_id: "me",
    workspace_id: "me-0",
    type: "volume",
    provider: "cluster",
    name: "scratch",
    slug: "st-333333333333",
    size_gi: 5,
    desired: "present",
    phase: "Ready",
    message: "the claim binds on first use",
    endpoint: "",
    connected: true,
    used_bytes: null,
    used_bytes_at: null,
    mounted_by: [],
  },
];

const providers = [
  { type: "volume", provider: "cluster", label: "Volume — cluster disk", available: true },
  { type: "object", provider: "s3", label: "Bucket — AWS S3", available: false, unavailable_reason: "lake#664" },
];

const services = [
  {
    id: "me-0-zetta",
    customer_id: "me",
    workspace_id: "me-0",
    kind: "zetta",
    name: "ada",
    default: true,
    instances: 1,
    storage_gi: 5,
    desired: "present",
    phase: "Ready",
    status: "Ready",
    message: "service ready",
    endpoint: "http://zetta.ws:4800",
    connected: true,
    stores: [],
  },
  {
    id: "svc-1",
    customer_id: "me",
    workspace_id: "me-0",
    kind: "sqlite",
    name: "orders-db",
    instances: 1,
    storage_gi: 10,
    desired: "present",
    phase: "Ready",
    status: "Ready",
    message: "service ready",
    endpoint: "http://orders-db:8080",
    connected: true,
    stores: [
      { store_id: "st-1", name: "uploads", type: "volume", provider: "cluster", mount_path: "/var/lib/data", read_only: false },
      { store_id: "st-2", name: "media", type: "object", provider: "s3", mount_path: "", read_only: false },
    ],
  },
  {
    id: "svc-2",
    customer_id: "me",
    workspace_id: "me-0",
    kind: "postgres",
    name: "crm",
    instances: 1,
    storage_gi: 10,
    desired: "present",
    phase: "Ready",
    status: "Ready",
    message: "service ready",
    endpoint: "postgres://crm:5432",
    connected: true,
    stores: [],
  },
];

const kinds = [
  { kind: "zetta", label: "Zetta", ready_seconds: 30, mounts_stores: false },
  { kind: "postgres", label: "Postgres", ready_seconds: 45, mounts_stores: false },
  { kind: "sqlite", label: "SQLite", ready_seconds: 25, mounts_stores: true },
];

const app = {
  id: "app-1",
  customer_id: "me",
  workspace_id: "me-0",
  name: "storefront",
  mode: "image",
  image: "registry/storefront:v3",
  framework: "node",
  port: 3000,
  replicas: 2,
  host: "storefront.apps.example.test",
  auth: "lake",
  bindings: { zetta: null, services: ["svc-2"] },
  env: {},
  secret_env_refs: {},
  desired: "running",
  generation: 4,
  phase: "Running",
  url: "https://storefront.apps.example.test",
  ready: true,
  message: "",
  stores: [{ store_id: "st-2", name: "media", type: "object", provider: "s3", mount_path: "", read_only: true }],
};

function answer(name) {
  if (name === "viewer") return viewer;
  if (name === "workspace_status") return { requested: true, ready: true, phase: "Ready", workspace_id: "me-0" };
  if (name === "list_stores") return { stores };
  if (name === "list_store_providers") return { providers };
  if (name === "list_services") return { services, limit: 0 };
  if (name === "list_service_kinds") return { kinds };
  if (name === "get_app") return app;
  if (name === "list_apps") return { apps: [app] };
  if (name === "list_drives") return { drives: [] };
  if (name === "usage_over_time") return { from: "2026-10-01T00:00:00Z", to: "2026-11-01T00:00:00Z", granularity: "month", periods: ["2026-10-01T00:00:00Z"], dimensions: [], workspaces: [], not_measured: [] };
  if (name === "list_builds") return { builds: [] };
  if (name === "app_logs") return { items: [] };
  if (name === "receive_app_events") return { events: [], next_sequence: 0, retained: 0 };
  return {};
}

const SHOTS = [
  { tag: "storage", path: "/w/me-0/drives?tab=storage", check: "table" },
  { tag: "storage-delete", path: "/w/me-0/drives?tab=storage", open: { role: "button", name: "Delete", nth: 0 }, check: "dialog" },
  { tag: "services", path: "/services?customer_id=me", check: "table" },
  { tag: "services-search", path: "/services?customer_id=me&q=orders-db", check: "table" },
  { tag: "service-storage", path: "/services?customer_id=me", open: { role: "button", name: "Storage orders-db mounts" }, check: "dialog" },
  { tag: "app-storage", path: "/w/me-0/ship/app?customer_id=me&app_id=app-1&tab=services", open: { role: "button", name: "Choose storage" }, check: "main" },
];

const browser = await chromium.launch();
const report = [];
for (const shot of SHOTS) {
  for (const width of [1440, 390]) {
    const context = await browser.newContext({ viewport: { width, height: width > 600 ? 900 : 844 }, deviceScaleFactor: width > 600 ? 1 : 2, colorScheme: "light" });
    await context.route(/\/v1\/[a-z_]+/, async (route) => {
      const name = new URL(route.request().url()).pathname.split("/v1/")[1];
      return route.fulfill({ contentType: "application/json", body: JSON.stringify({ ok: true, value: answer(name) }) });
    });
    const page = await context.newPage();
    const errors = [];
    page.on("pageerror", (e) => errors.push(String(e)));
    page.on("console", (m) => m.type() === "error" && errors.push(m.text().slice(0, 300)));
    await page.goto(BASE + shot.path, { waitUntil: "load" });
    await page.waitForTimeout(1200);
    await page.waitForTimeout(500);
    if (shot.open) {
      try {
        await page.getByRole(shot.open.role, { name: shot.open.name, exact: true }).nth(shot.open.nth ?? 0).click({ timeout: 5000 });
      } catch (error) {
        errors.push(`could not open: ${String(error).split("\n")[0]}`);
      }
      await page.waitForTimeout(500);
    }
    await page.screenshot({ path: `${OUT}/${shot.tag}-${width}.png`, fullPage: true });
    const scope = shot.check === "dialog" ? page.locator('[role="dialog"]') : page.locator("main");
    report.push({
      shot: shot.tag,
      width,
      sideways: await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth),
      text: ((await scope.count()) ? await scope.first().innerText() : "").replace(/\s+/g, " ").slice(0, 600),
      errors,
    });
    await context.close();
  }
}
await browser.close();
console.log(JSON.stringify(report, null, 2));
