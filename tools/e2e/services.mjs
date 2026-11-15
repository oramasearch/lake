// SPDX-License-Identifier: MIT

// Every service kind through lake's Services page (docs/api/services.md): New service, wait for
// Ready (reporting how long it took against the kind's start time in the menu), Show connection
// (field names only), delete with the typed name, wait until gone. Passwords and tokens are never
// printed or screenshotted.
//
//   node tools/e2e/services.mjs <base-url> <ticket-file> <output-dir> [kind ...]
//
// Kinds default to every kind in the menu but zetta. Postgres is also connected from Data →
// Connections (Connect One of Your Services, `connect_service`) and its tables listed through lake
// (`list_source_tables`), then the connection is deleted again: the check that needs no access to
// the cluster.
//
// Optional, off by default: with LAKE_E2E_PORT_FORWARD=1 and LAKE_KUBE_CONTEXT (and LAKE_NAMESPACE,
// default lake), our wrapper kinds (sqlite, duckdb, polars) also run real queries, and a sandbox
// escape that must be refused, through `kubectl port-forward`. A gate run that may only read the
// cluster leaves it off. lib.mjs finds Playwright; a new account gets its first top-up and
// workspace there. Not C++: keep it out of git clang-format.
import { spawn } from "node:child_process";
import { args, wa, wq, ensureFunds, ensureWorkspace, errorText, inWorkspace, loadChromium, logger, newPage, shooter, signIn, until } from "./lib.mjs";

const { base, ticketFile, out, rest: kindsArg } = args("node tools/e2e/services.mjs <base-url> <ticket-file> <output-dir> [kind ...]");
const forward = process.env.LAKE_E2E_PORT_FORWARD === "1" && process.env.LAKE_KUBE_CONTEXT;
const chromium = await loadChromium();
const log = logger(out);
const { note, check, failed } = log;
const { browser, page } = await newPage(chromium, base);
const shot = shooter(page, out);
const dialog = () => page.getByRole("dialog");
const table = () => page.getByRole("table", { name: "Services" });
// The status cell (data-testid since lake#1099; the fourth cell on a Lake from before).
const statusOf = async (row) =>
  (await row.getByTestId("service-status").or(row.getByRole("cell").nth(3)).first().innerText().catch(() => "")).replace(/\s+/g, " ").replace(/^[^A-Za-z]+/, "");
const seconds = (label) => {
  const m = label.match(/about (\d+) (second|minute)/);
  return m ? Number(m[1]) * (m[2] === "minute" ? 60 : 1) : 0;
};

// Runs `body(call)` against the service through a port-forward; `call(path, body, headers)` posts
// JSON unless the body is already a string.
async function throughForward(connection, body) {
  const host = new URL(connection.endpoint).hostname.split(".")[0];
  const child = spawn("kubectl", [`--context=${process.env.LAKE_KUBE_CONTEXT}`, "-n", process.env.LAKE_NAMESPACE ?? "lake", "port-forward", `svc/${host}`, "18099:8080"], { stdio: "ignore" });
  try {
    await new Promise((resolve) => setTimeout(resolve, 4000));
    const call = async (path, payload, headers = { "Content-Type": "application/json" }) => {
      const res = await fetch(`http://127.0.0.1:18099${path}`, { method: "POST", headers: { ...headers, Authorization: `Bearer ${connection.token}` }, body: typeof payload === "string" ? payload : JSON.stringify(payload) });
      return { status: res.status, body: await res.json() };
    };
    return await body(call);
  } finally {
    child.kill();
  }
}

const queries = {
  sqlite: async (call) => {
    await call("/v1/exec", { sql: "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)" });
    await call("/v1/exec", { sql: "INSERT INTO t (v) VALUES (?)", params: ["ok"] });
    const rolled = await call("/v1/batch", { statements: [{ sql: "INSERT INTO t (v) VALUES ('no')" }, { sql: "INSERT INTO missing VALUES (1)" }] });
    const selected = await call("/v1/query", { sql: "SELECT v FROM t" });
    return { passed: rolled.status === 400 && JSON.stringify(selected.body.rows) === JSON.stringify([["ok"]]), rows: selected.body.rows };
  },
  // The batch must roll back whole, and the lockdown must refuse reading a file on the host.
  duckdb: async (call) => {
    await call("/v1/exec", { sql: "CREATE TABLE t (id INTEGER, v VARCHAR)" });
    await call("/v1/exec", { sql: "INSERT INTO t VALUES (?, ?)", params: [1, "ok"] });
    const rolled = await call("/v1/batch", { statements: [{ sql: "INSERT INTO t VALUES (2, 'no')" }, { sql: "INSERT INTO missing VALUES (1)" }] });
    const selected = await call("/v1/query", { sql: "SELECT v FROM t ORDER BY id" });
    const escape = await call("/v1/query", { sql: "SELECT * FROM read_csv('/etc/passwd')" });
    return {
      passed: rolled.status === 400 && JSON.stringify(selected.body.rows) === JSON.stringify([["ok"]]) && escape.status === 403,
      rows: selected.body.rows, batch_status: rolled.status, file_read_status: escape.status,
    };
  },
  // Upload a dataset, aggregate it, and the SQL sandbox must refuse a table function.
  polars: async (call) => {
    const uploaded = await call("/v1/datasets/orders", '{"id":1,"amount":2.5}\n{"id":2,"amount":4}\n', { "Content-Type": "application/x-ndjson" });
    const summed = await call("/v1/query", { sql: "SELECT count(*) AS n, sum(amount) AS total FROM orders" });
    const escape = await call("/v1/query", { sql: "SELECT * FROM read_csv('/etc/passwd')" });
    return {
      passed: uploaded.status === 201 && JSON.stringify(summed.body.rows) === JSON.stringify([[2, 6.5]]) && escape.status === 400,
      rows: summed.body.rows, upload_status: uploaded.status, table_function_status: escape.status,
    };
  },
};

/** Postgres from Data → Connections: connect it in one click, list its tables through lake, then
 * delete the connection with its typed name. */
async function connectPostgres(viewer, workspace, name) {
  await page.goto(inWorkspace(base, workspace, "/connections"));
  const item = page.getByRole("listitem").filter({ hasText: name });
  await item.getByRole("button", { name: /^(Connect|Refresh)$/ }).click();
  const listed = await until(page, async () => (await page.getByRole("table").filter({ hasText: name }).count()) > 0, { minutes: 1, every: 1000 });
  const tables = await wq(page, base, viewer.customer_id, workspace, "list_source_tables", { connection: name })
    .then((value) => ({ ok: true, tables: (value.tables ?? []).length }))
    .catch((error) => ({ ok: false, error: error.message }));
  await shot(`${name}-connected`);
  check("postgres connect", listed && tables.ok, { listed, ...tables, page_error: await errorText(page) });
  await wa(page, base, viewer.customer_id, workspace, "delete_connection", { name })
    .catch((error) => note("postgres disconnect", { error: error.message }));
}

try {
  const viewer = await signIn(page, base, ticketFile);
  note("sign-in", { url: page.url() });
  await ensureFunds(page, base, log);
  const workspace = await ensureWorkspace(page, base, log);
  if (!workspace) throw new Error("no ready workspace");

  await page.goto(`${base}/services`);
  await page.getByRole("button", { name: "New service" }).first().click();
  const menu = await dialog().getByRole("combobox", { name: "Kind" }).evaluate((select) => [...select.options].map((o) => ({ kind: o.value, label: o.textContent ?? "" })));
  await dialog().getByRole("button", { name: "Cancel" }).click();
  const kinds = kindsArg.length ? kindsArg : menu.map((m) => m.kind).filter((k) => k !== "zetta");
  note("kinds", { menu: menu.map((m) => m.kind), testing: kinds, queries: forward ? "through a port-forward" : "off (LAKE_E2E_PORT_FORWARD unset)" });

  for (const kind of kinds) {
    const entry = menu.find((m) => m.kind === kind);
    if (!entry) { check(`${kind} offered`, false, { error: "not in the kind menu" }); continue; }
    const expected = seconds(entry.label);
    const name = `t-${kind}-${Date.now().toString(36).slice(-4)}`;
    const row = () => table().getByRole("row").filter({ hasText: name });
    await page.goto(`${base}/services`);
    await page.getByRole("button", { name: "New service" }).first().click();
    await dialog().getByRole("combobox", { name: "Kind" }).selectOption(kind);
    await dialog().getByRole("textbox", { name: "Name" }).fill(name);
    await dialog().getByRole("spinbutton", { name: "Storage (GiB)" }).fill("1");
    const started = Date.now();
    await dialog().getByRole("button", { name: "Create service" }).click();
    await dialog().waitFor({ state: "detached", timeout: 30000 }).catch(() => {});
    if (await dialog().count()) {
      check(`${kind} created`, false, { error: (await dialog().innerText()).replace(/\s+/g, " ").slice(0, 300) });
      await dialog().getByRole("button", { name: "Cancel" }).click();
      continue;
    }
    // The page checks again every 3 seconds while a service is changing.
    let status = "";
    await until(page, async () => {
      status = (await row().count()) ? await statusOf(row()) : (await table().count()) ? "missing" : `not listed: ${await errorText(page)}`;
      return /^(Ready|Failed)/.test(status);
    }, { minutes: Math.max(20, (expected / 60) * 3), every: 3000 });
    const took = Math.round((Date.now() - started) / 1000);
    await shot(`${kind}-status`);
    check(`${kind} ready`, status.startsWith("Ready"), { status, seconds: took, expected_seconds: expected, within_expected: took <= expected });

    if (status.startsWith("Ready")) {
      await row().getByRole("button", { name: "Show connection" }).click();
      await dialog().waitFor();
      const keys = await dialog().locator("dl dt").allInnerTexts();
      const values = await dialog().locator("dl dd").allInnerTexts();
      check(`${kind} connection`, keys.length > 0, { fields: keys });
      const connection = Object.fromEntries(keys.map((k, i) => [k, values[i]]));
      await dialog().getByRole("button", { name: "Close" }).first().click();
      if (forward && queries[kind]) {
        const result = await throughForward(connection, queries[kind]);
        check(`${kind} queries`, result.passed, result);
      }
      if (kind === "postgres") await connectPostgres(viewer, workspace, name);
    }

    await page.goto(`${base}/services`);
    await row().getByRole("button", { name: `Delete ${name}` }).click();
    await dialog().getByRole("textbox").fill(name);
    await dialog().getByRole("button", { name: `Delete ${name}` }).click();
    const deleteStarted = Date.now();
    // Gone means the table is listed without it (not an error panel in its place).
    const gone = await until(page, async () => (await table().count()) > 0 && (await row().count()) === 0, { minutes: 10, every: 3000 });
    check(`${kind} deleted`, gone, { seconds: Math.round((Date.now() - deleteStarted) / 1000) });
  }
} catch (error) {
  check("error", false, { message: String(error).slice(0, 300), url: page.url() });
  await shot("failure");
} finally {
  await browser.close();
  process.exit(failed() ? 1 : 0);
}
