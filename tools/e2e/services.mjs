// Every service kind through lake's Services page (docs/api/services.md): create, wait for Ready
// (reporting how long it took against the kind's expected start time), Show connection, delete,
// wait until gone. Passwords and tokens are never printed or screenshotted.
//
//   node tools/e2e/services.mjs <base-url> <ticket-file> <output-dir> [kind ...]
//
// Kinds default to every kind but zetta. For sqlite it also runs real SQL through a port-forward
// (needs kubectl and the cluster context in LAKE_KUBE_CONTEXT, default orama-production-cluster-01).
// Playwright is found as walkthrough.mjs finds it. Not C++: keep it out of git clang-format.
import fs from "node:fs";
import { spawn } from "node:child_process";

const candidates = [
  process.env.PLAYWRIGHT,
  "playwright",
  new URL("../../../../../orama-website-v2/node_modules/playwright/index.mjs", import.meta.url).href,
].filter(Boolean);
let chromium;
for (const spec of candidates) { try { ({ chromium } = await import(spec)); break; } catch {} }
if (!chromium) { console.error(`Playwright not found. Tried:\n  ${candidates.join("\n  ")}`); process.exit(1); }

const [base, ticketFile, out, ...kindsArg] = process.argv.slice(2);
const kinds = kindsArg.length ? kindsArg : ["postgres", "kafka", "clickhouse", "valkey", "tidb", "cassandra", "sqlite"];
const context = process.env.LAKE_KUBE_CONTEXT ?? "orama-production-cluster-01";
fs.mkdirSync(out, { recursive: true });
const log = [];
const note = (step, detail) => {
  log.push({ step, time: new Date().toISOString(), ...detail });
  console.log(new Date().toISOString().slice(11, 19), step, JSON.stringify(detail));
  fs.writeFileSync(`${out}/log.json`, JSON.stringify(log, null, 2));
};

const browser = await chromium.launch();
const page = await (await browser.newContext({ viewport: { width: 1440, height: 900 } })).newPage();
const shot = (name) => page.screenshot({ path: `${out}/${name}.png`, fullPage: true });
const errorText = async () => (await page.locator("main .error").allInnerTexts()).join(" | ");
let failed = false;

async function sqliteQueries(connection) {
  const host = new URL(connection.endpoint).hostname.split(".")[0];
  const forward = spawn("kubectl", [`--context=${context}`, "-n", "lake", "port-forward", `svc/${host}`, "18099:8080"], { stdio: "ignore" });
  try {
    await new Promise((resolve) => setTimeout(resolve, 4000));
    const call = async (path, body) => {
      const res = await fetch(`http://127.0.0.1:18099${path}`, { method: "POST", headers: { "Content-Type": "application/json", Authorization: `Bearer ${connection.token}` }, body: JSON.stringify(body) });
      return { status: res.status, body: await res.json() };
    };
    await call("/v1/exec", { sql: "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)" });
    await call("/v1/exec", { sql: "INSERT INTO t (v) VALUES (?)", params: ["ok"] });
    const rolled = await call("/v1/batch", { statements: [{ sql: "INSERT INTO t (v) VALUES ('no')" }, { sql: "INSERT INTO missing VALUES (1)" }] });
    const selected = await call("/v1/query", { sql: "SELECT v FROM t" });
    const passed = rolled.status === 400 && JSON.stringify(selected.body.rows) === JSON.stringify([["ok"]]);
    note("sqlite queries", { passed, rows: selected.body.rows });
    if (!passed) failed = true;
  } finally {
    forward.kill();
  }
}

try {
  await page.goto(`${base}/sign-in?__clerk_ticket=${encodeURIComponent(fs.readFileSync(ticketFile, "utf8").trim())}`);
  await page.waitForURL(`${base}/repositories**`, { timeout: 60000 });
  for (const kind of kinds) {
    const name = `t-${kind}-${Date.now().toString(36).slice(-4)}`;
    const row = () => page.locator("tr", { hasText: name });
    await page.goto(`${base}/services`);
    const option = page.locator(`select[name=kind] option[value='${kind}']`);
    if (!(await option.count())) { note(`${kind} missing`, { error: "not in the kind menu" }); failed = true; continue; }
    const expected = Number(((await option.innerText()).match(/about (\d+) minute/) ?? [])[1] ?? 0);
    await page.selectOption("select[name=kind]", kind);
    await page.fill("form[action='/services/create'] input[name=name]", name);
    await page.fill("form[action='/services/create'] input[name=storage_gi]", "1");
    const started = Date.now();
    await page.getByRole("button", { name: "Create service" }).click();
    await page.waitForLoadState("load");
    let status = "";
    while (Date.now() - started < Math.max(20, expected * 3) * 60 * 1000) {
      await page.goto(`${base}/services`);
      status = (await row().count()) ? (await row().locator("td").nth(3).innerText()).replace(/\s+/g, " ") : "missing";
      if (status.startsWith("Ready") || status.startsWith("Failed") || status === "missing") break;
      await page.waitForTimeout(10000);
    }
    const seconds = Math.round((Date.now() - started) / 1000);
    await shot(`${kind}-status`);
    note(`${kind} ready`, { status, seconds, expected_minutes: expected, within_expected: seconds <= expected * 60 });
    if (!status.startsWith("Ready")) failed = true;
    if (status.startsWith("Ready") && kind !== "zetta") {
      await row().getByRole("button", { name: "Show connection" }).click();
      await page.waitForLoadState("load");
      const keys = await page.locator("main dl dt").allInnerTexts();
      const values = await page.locator("main dl dd").allInnerTexts();
      const connection = Object.fromEntries(keys.map((k, i) => [k, values[i]]));
      note(`${kind} connection`, { fields: keys, error: await errorText() });
      if (kind === "sqlite") await sqliteQueries(connection);
    }
    await page.goto(`${base}/services`);
    if (await row().count()) {
      await row().locator("details summary").click();
      await row().locator("input[name=confirm]").fill(name);
      await row().getByRole("button", { name: `Delete ${name}` }).click();
      await page.waitForLoadState("load");
      const deleteStarted = Date.now();
      while (Date.now() - deleteStarted < 10 * 60 * 1000 && (await row().count())) { await page.waitForTimeout(10000); await page.goto(`${base}/services`); }
      const gone = (await row().count()) === 0;
      note(`${kind} deleted`, { gone, seconds: Math.round((Date.now() - deleteStarted) / 1000) });
      if (!gone) failed = true;
    }
  }
} catch (error) {
  await shot("failure");
  note("failure", { message: String(error).slice(0, 300), url: page.url() });
  failed = true;
} finally {
  await browser.close();
  process.exitCode = failed ? 1 : 0;
}
