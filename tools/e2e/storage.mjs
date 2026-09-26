// Train → Data → Storage in a browser (docs/api/storage.md, lake#196): create each available
// store type and provider on the Data page, wait for Ready, show its connection (field names only),
// check the Kubernetes object, delete it with the typed confirmation, and check it is gone.
// Unavailable pairs must be listed disabled with a reason.
//
//   node tools/e2e/storage.mjs <base-url> <ticket-file> <output-dir>
//
// kubectl reaches the cluster through LAKE_KUBE_CONTEXT (default orama-production-cluster-01),
// the Lake's namespace LAKE_NAMESPACE (default lake). Playwright is found as walkthrough.mjs finds
// it. Not C++: keep it out of git clang-format.
import fs from "node:fs";
import { spawnSync } from "node:child_process";

const candidates = [
  process.env.PLAYWRIGHT,
  "playwright",
  new URL("../../../../../orama-website-v2/node_modules/playwright/index.mjs", import.meta.url).href,
].filter(Boolean);
let chromium;
for (const spec of candidates) { try { ({ chromium } = await import(spec)); break; } catch {} }
if (!chromium) { console.error(`Playwright not found. Tried:\n  ${candidates.join("\n  ")}`); process.exit(1); }

const [base, ticketFile, out] = process.argv.slice(2);
const context = process.env.LAKE_KUBE_CONTEXT ?? "orama-production-cluster-01";
const namespace = process.env.LAKE_NAMESPACE ?? "lake";
fs.mkdirSync(out, { recursive: true });
const log = [];
let failed = false;
const note = (step, detail) => {
  log.push({ step, time: new Date().toISOString(), ...detail });
  console.log(new Date().toISOString().slice(11, 19), step, JSON.stringify(detail));
  fs.writeFileSync(`${out}/log.json`, JSON.stringify(log, null, 2));
};
const check = (step, passed, detail = {}) => { note(step, { passed, ...detail }); if (!passed) failed = true; };
const kubectl = (...args) => spawnSync("kubectl", [`--context=${context}`, "-n", namespace, ...args], { encoding: "utf8" });

const browser = await chromium.launch();
const page = await (await browser.newContext({ viewport: { width: 1440, height: 900 } })).newPage();
const shot = (name) => page.screenshot({ path: `${out}/${name}.png`, fullPage: true });
const storage = () => page.locator("#storage");
try {
  await page.goto(`${base}/sign-in?__clerk_ticket=${encodeURIComponent(fs.readFileSync(ticketFile, "utf8").trim())}`);
  await page.waitForURL(`${base}/repositories**`, { timeout: 60000 });
  await page.goto(`${base}/train/data`);
  await shot("storage");
  const options = await storage().locator("select[name=store] option").evaluateAll((all) =>
    all.map((o) => ({ value: o.value, text: o.textContent, disabled: o.disabled })));
  const available = options.filter((o) => !o.disabled);
  check("providers", available.length > 0 && options.filter((o) => o.disabled).every((o) => o.text.includes("not yet")),
        { available: available.map((o) => o.value), unavailable: options.length - available.length });

  for (const option of available) {
    const name = `e2e-${option.value.replace(/[^a-z]/g, "")}-${Date.now().toString(36).slice(-5)}`;
    const row = () => storage().locator("tr", { hasText: name });
    await page.goto(`${base}/train/data`);
    await storage().locator("select[name=store]").selectOption(option.value);
    await storage().locator("input[name=name]").fill(name);
    await storage().locator("input[name=size_gi]").fill("1");
    const created = Date.now();
    await storage().getByRole("button", { name: "Create storage" }).click();
    await page.waitForLoadState("load");
    let status = "";
    while (Date.now() - created < 5 * 60 * 1000) {
      await page.goto(`${base}/train/data`);
      status = (await row().locator("td").nth(4).innerText().catch(() => "")).replace(/\s+/g, " ");
      if (/^(Ready|Failed)/.test(status)) break;
      await page.waitForTimeout(2000);
    }
    await shot(`${option.value.replace(":", "-")}-ready`);
    check(`${option.value} ready`, status.startsWith("Ready"), { status, seconds: Math.round((Date.now() - created) / 1000) });

    await row().getByRole("button", { name: "Show connection" }).click();
    await page.waitForLoadState("load");
    const fields = await page.locator("main dl dt").allInnerTexts();
    const values = await page.locator("main dl dd").allInnerTexts();
    check(`${option.value} connection`, fields.length > 0, { fields });
    if (option.value === "volume:cluster") {
      const claimName = values[fields.indexOf("claim")] ?? "";
      const claim = JSON.parse(kubectl("get", "pvc", claimName, "-o", "json").stdout || "null");
      check("volume claim", !!claim && claimName !== "",
            { claim: claimName, size: claim?.spec.resources.requests.storage, phase: claim?.status.phase });
    }

    await page.goto(`${base}/train/data`);
    await row().locator("details summary").click();
    await row().locator("input[name=confirm]").fill(name);
    await row().getByRole("button", { name: `Delete ${name}` }).click();
    await page.waitForLoadState("load");
    const deleting = Date.now();
    let gone = false;
    while (Date.now() - deleting < 3 * 60 * 1000) {
      await page.goto(`${base}/train/data`);
      if (!(await row().count())) { gone = true; break; }
      await page.waitForTimeout(2000);
    }
    check(`${option.value} deleted`, gone, { seconds: Math.round((Date.now() - deleting) / 1000) });
  }
} catch (error) {
  failed = true;
  note("error", { message: String(error).slice(0, 300) });
} finally {
  await browser.close();
  process.exit(failed ? 1 : 0);
}
