// SPDX-License-Identifier: MIT

// Screenshots of Data → Drives → Storage (lake#664, docs/ui/drives.md#storage) at 1440 and 390
// wide: the table with a volume, your own bucket and a store Lake keeps, New storage for each kind
// of provider, and the delete warnings. Run against `npx vite --port 5175`; every /v1 call is
// answered by the mocks below, so no lake is needed. Playwright is not a dependency of the app:
//
//   PLAYWRIGHT_FROM=/path/to/a/project/with/playwright/package.json OUT=/tmp/shots node checks/storage.mjs
//
// It prints a JSON report: per width and step, whether the page scrolls sideways, the dialog's
// field labels, and any page error.
import { createRequire } from "node:module";
import { mkdirSync } from "node:fs";
const require = createRequire(process.env.PLAYWRIGHT_FROM ?? import.meta.url);
const { chromium } = require("playwright");

const OUT = process.env.OUT ?? "/tmp/lake-storage-shots";
mkdirSync(OUT, { recursive: true });
const BASE = process.env.BASE ?? "http://localhost:5175";

const viewer = {
  signed_in: true, administrator: false, display_name: "Ada Lovelace", email: "ada@example.com", user_id: "user-1",
  customer_id: "me", workspace_id: "ws-1",
  spaces: [{ id: "me", name: "ada-x7f2", label: "Your workspaces", personal: true, kind: "individual", workspaces: [{ id: "ws-1", name: "Support", customer_id: "me", status: "ready" }] }],
  clerk: { enabled: false }, password_sign_up: false, version: "dev",
};
const providers = [
  ["volume", "cluster", "Volume — cluster disk"],
  ["object", "r2", "Bucket — your Cloudflare R2 bucket"],
  ["object", "s3", "Bucket — your AWS S3 bucket"],
  ["object", "b2", "Bucket — your Backblaze B2 bucket"],
  ["object", "gcs", "Bucket — your Google Cloud Storage bucket"],
  ["object", "azure", "Bucket — your Azure Blob container"],
  ["object", "zetta", "Bucket — kept by Lake in the workspace"],
  ["object", "lake", "Bucket — a liquid repository"],
  ["filesystem", "zetta", "Filesystem — kept by Lake in the workspace"],
  ["filesystem", "lake", "Filesystem — Drive (a liquid repository)"],
].map(([type, provider, label]) => ({ type, provider, label, available: true }))
  .concat([{ type: "network_filesystem", provider: "nfs", label: "Network filesystem — NFS in the cluster", available: false, unavailable_reason: "phase 4" }]);
const base = { customer_id: "me", workspace_id: "ws-1", desired: "present", generation: 1, observed_generation: 1, connected: true, public: false, region: "", created_at: "2026-10-11T10:00:00Z" };
const stores = [
  { ...base, id: "s-1", name: "scratch", type: "volume", provider: "cluster", slug: "st-aaaaaaaaaaaa", size_gi: 10, phase: "Ready", message: "created; binds when a service mounts it", endpoint: "ws-support/prod-st-aaaaaaaaaaaa", location: null },
  { ...base, id: "s-2", name: "media", type: "object", provider: "r2", slug: "st-bbbbbbbbbbbb", size_gi: 0, phase: "Ready", message: "credentials delivered", endpoint: "https://0123456789abcdef.r2.cloudflarestorage.com/media", location: { bucket: "media", endpoint: "https://0123456789abcdef.r2.cloudflarestorage.com", region: "auto", prefix: "team/shared", credential: { mount_id: "m-1", name: "r2-keys" } } },
  { ...base, id: "s-3", name: "notes", type: "filesystem", provider: "zetta", slug: "st-cccccccccccc", size_gi: 0, phase: "Ready", message: "kept by Lake in the workspace", endpoint: "", location: null },
  { ...base, id: "s-4", name: "models", type: "object", provider: "lake", slug: "st-dddddddddddd", size_gi: 0, phase: "Provisioning", message: "", endpoint: "", location: { repository_id: "r-1" } },
];
const answers = {
  viewer,
  workspace_status: { requested: true, ready: true, phase: "Ready", workspace_id: "ws-1" },
  list_workspaces: { workspaces: [{ id: "ws-1", name: "Support", environment: "dev", phase: "Ready", desired: "running", storage_gi: 5 }] },
  list_store_providers: { providers },
  list_stores: { stores },
  list_drives: { drives: [{ id: "d-1", name: "Media", storage: "store", store_id: "s-2" }] },
  list_secret_mounts: { items: [{ id: "m-1", name: "bucket-keys" }] },
  list_repositories: { items: [{ id: "r-1", name: "models", workspace_id: "ws-1" }, { id: "r-2", name: "datasets", workspace_id: "ws-1" }] },
  usage_over_time: { dimensions: [{ dimension: "volumes", title: "Volumes", unit: "GiB-hours", by_object: [{ key: "s-1", total: 1.5 }] }] },
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
        fields: [...document.querySelectorAll('[role="dialog"] label')].map((l) => l.textContent?.trim().slice(0, 40)).filter(Boolean),
        banner: document.querySelector('[role="dialog"] [role="status"], [role="dialog"] .banner')?.textContent?.trim() ?? null,
      }))),
      errors: [...errors],
    });
  };
  await page.goto(BASE + "/w/ws-1/drives?tab=storage", { waitUntil: "networkidle" });
  await page.waitForTimeout(500);
  await facts("table");
  for (const [tag, value] of [["new-volume", "volume:cluster"], ["new-r2", "object:r2"], ["new-azure", "object:azure"], ["new-lake", "filesystem:lake"], ["new-zetta", "filesystem:zetta"]]) {
    await page.getByRole("button", { name: "New storage" }).first().click();
    await page.waitForTimeout(250);
    await page.locator('select[name="store"]').selectOption(value);
    await page.waitForTimeout(250);
    await facts(tag);
    await page.getByRole("button", { name: "Cancel" }).click();
    await page.waitForTimeout(150);
  }
  for (const [tag, row] of [["delete-bucket", 1], ["delete-volume", 0]]) {
    await page.getByRole("button", { name: "Delete", exact: true }).nth(row).click();
    await page.waitForTimeout(250);
    await facts(tag);
    await page.getByRole("button", { name: "Cancel" }).click();
    await page.waitForTimeout(150);
  }
  await context.close();
}
await browser.close();
console.log(JSON.stringify(report, null, 2));
