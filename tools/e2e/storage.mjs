// SPDX-License-Identifier: MIT

// Data → Drives → Storage in a browser (docs/api/storage.md, docs/ui/drives.md#storage, lake#664):
// for each type and provider New storage offers that Lake can create without a bucket of the
// person's own, create a store (a volume of 1 GiB), wait for Ready, show its connection (field names
// only), delete it with the typed confirmation, and check it is gone. Providers that are not
// available yet must be left out of the menu with a reason in list_store_providers. Providers that
// need the customer's own bucket and a secret (R2, S3, B2, GCS, Azure) are listed as skipped. The
// repository a `lake` store made is removed after the store (`delete_repository`), since it stays.
//
//   node tools/e2e/storage.mjs <base-url> <ticket-file> <output-dir>
//
// Optional: with LAKE_KUBE_CONTEXT and LAKE_NAMESPACE set (the cluster and namespace the Lake's
// workspaces run in), a volume's claim is also read with `kubectl get pvc` (read-only). lib.mjs
// finds Playwright; a new account gets its first top-up and workspace there. Not C++: keep it out
// of git clang-format.
import { spawnSync } from "node:child_process";
import { api, args, ensureFunds, ensureWorkspace, errorText, loadChromium, logger, newPage, shooter, signIn, until } from "./lib.mjs";

const { base, ticketFile, out } = args("node tools/e2e/storage.mjs <base-url> <ticket-file> <output-dir>");
const kubeContext = process.env.LAKE_KUBE_CONTEXT;
const namespace = process.env.LAKE_NAMESPACE;
const chromium = await loadChromium();
const log = logger(out);
const { note, check, failed } = log;
const { browser, page } = await newPage(chromium, base);
const shot = shooter(page, out);
const dialog = () => page.getByRole("dialog");
const storage = () => page.getByRole("region", { name: "Storage" });
// The status cell (data-testid since lake#1099; the fifth cell on a Lake from before).
const statusOf = async (row) =>
  (await row.getByTestId("store-status").or(row.getByRole("cell").nth(4)).first().innerText().catch(() => "")).replace(/\s+/g, " ").replace(/^[^A-Za-z]+/, "");

try {
  const viewer = await signIn(page, base, ticketFile);
  note("sign-in", { url: page.url() });
  await ensureFunds(page, base, log);
  const workspace = await ensureWorkspace(page, base, log);
  if (!workspace) throw new Error("no ready workspace");

  // Storage is a tab of Data → Drives; /storage opens it in the current workspace.
  await page.goto(`${base}/storage`);
  await storage().waitFor();
  await shot("storage");
  check("page", /\/drives\?tab=storage/.test(page.url()), { url: page.url() });

  const providers = (await api(page, base, "list_store_providers", { customer_id: viewer.customer_id })).providers ?? [];
  await storage().getByRole("button", { name: "New storage" }).first().click();
  const options = await dialog().getByRole("combobox", { name: "Type and provider" }).evaluate((select) =>
    [...select.options].map((o) => ({ value: o.value, text: o.textContent, disabled: o.disabled })));
  const offered = new Set(options.map((o) => o.value));
  const unavailable = providers.filter((p) => p.available === false);
  check("providers", options.length > 0 && unavailable.every((p) => !offered.has(`${p.type}:${p.provider}`) && !!p.unavailable_reason),
        { offered: [...offered], unavailable: unavailable.map((p) => `${p.type}:${p.provider} (${p.unavailable_reason ?? "no reason"})`) });
  await dialog().getByRole("button", { name: "Cancel" }).click();

  for (const option of options) {
    const name = `e2e-${option.value.replace(/[^a-z]/g, "")}-${Date.now().toString(36).slice(-5)}`.slice(0, 30);
    const row = () => storage().getByRole("row").filter({ hasText: name });
    await page.goto(`${base}/storage`);
    await storage().getByRole("button", { name: "New storage" }).first().click();
    await dialog().getByRole("combobox", { name: "Type and provider" }).selectOption(option.value);
    // A bucket of the customer's own needs its name and a secret that opens it: not this test's.
    if (await dialog().getByRole("textbox", { name: /^(Bucket|Container)/ }).count()) {
      note(`${option.value} skipped`, { reason: "needs the customer's own bucket and a secret mount" });
      await dialog().getByRole("button", { name: "Cancel" }).click();
      continue;
    }
    await dialog().getByRole("textbox", { name: "Name" }).fill(name);
    const size = dialog().getByRole("spinbutton", { name: "Size (GiB)" });
    if (await size.count()) await size.fill("1");
    const created = Date.now();
    await dialog().getByRole("button", { name: "Create storage" }).click();
    await dialog().waitFor({ state: "detached", timeout: 30000 }).catch(() => {});
    if (await dialog().count()) {
      check(`${option.value} created`, false, { error: await dialog().innerText().then((t) => t.replace(/\s+/g, " ").slice(0, 300)) });
      await dialog().getByRole("button", { name: "Cancel" }).click();
      continue;
    }
    let status = "";
    await until(page, async () => {
      status = (await row().count()) ? await statusOf(row()) : "";
      return /^(Ready|Failed)/.test(status);
    }, { minutes: 5, every: 2000 });
    await shot(`${option.value.replace(":", "-")}-ready`);
    check(`${option.value} ready`, status.startsWith("Ready"), { status, seconds: Math.round((Date.now() - created) / 1000) });

    if (status.startsWith("Ready")) {
      await row().getByRole("button", { name: "Show connection" }).click();
      await dialog().waitFor();
      const fields = await dialog().locator("dl dt").allInnerTexts();
      const values = await dialog().locator("dl dd").allInnerTexts();
      check(`${option.value} connection`, fields.length > 0, { fields, error: await errorText(page) });
      if (option.value === "volume:cluster" && kubeContext && namespace) {
        const claimName = (values[fields.indexOf("claim")] ?? "").trim();
        const claim = JSON.parse(spawnSync("kubectl", [`--context=${kubeContext}`, "-n", namespace, "get", "pvc", claimName, "-o", "json"], { encoding: "utf8" }).stdout || "null");
        check("volume claim", !!claim && claimName !== "", { claim: claimName, size: claim?.spec.resources.requests.storage, phase: claim?.status.phase });
      }
      await page.keyboard.press("Escape");
      await dialog().waitFor({ state: "detached", timeout: 5000 }).catch(() => {});
    }

    // A `lake` store's repository stays when the store goes (docs/api/storage.md); remember it to
    // remove it after, so the test leaves nothing behind.
    const stores = (await api(page, base, "list_stores", { customer_id: viewer.customer_id, workspace_id: workspace }, { workspace }).catch(() => ({}))).stores ?? [];
    const repository = stores.find((s) => s.name === name)?.location?.repository_id ?? "";
    await page.goto(`${base}/storage`);
    await row().getByRole("button", { name: "Delete", exact: true }).click();
    await dialog().getByRole("textbox").fill(name);
    await dialog().getByRole("button", { name: `Delete ${name}` }).click();
    const deleting = Date.now();
    const gone = await until(page, async () => {
      await page.goto(`${base}/storage`);
      await storage().waitFor();
      await page.waitForTimeout(1000);
      return (await row().count()) === 0;
    }, { minutes: 3, every: 2000 });
    check(`${option.value} deleted`, gone, { seconds: Math.round((Date.now() - deleting) / 1000) });
    if (repository) {
      const removed = await api(page, base, "delete_repository", { id: repository }, { post: true }).then(() => true).catch((error) => error.message);
      note(`${option.value} repository removed`, { repository, removed });
    }
  }
} catch (error) {
  check("error", false, { message: String(error).slice(0, 300), url: page.url() });
  await shot("failure");
} finally {
  await browser.close();
  process.exit(failed() ? 1 : 0);
}
