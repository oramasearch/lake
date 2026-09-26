// End-to-end browser walkthrough of lake (docs/deployment/browser-testing.md): Clerk sign-in with a
// one-time ticket, then every Design, Train and Ship page, with a screenshot per step and a
// log.json. It creates one project and one Postgres service in the signed-in customer's space and
// deletes both again, with any e2e- projects an earlier run left behind.
//
//   node tools/e2e/walkthrough.mjs <base-url> <ticket-file> <output-dir>
//
// Playwright comes from the sibling orama-website-v2 checkout, as the UI tools do; set PLAYWRIGHT to
// point at another playwright package. Not C++: keep it out of git clang-format.
import fs from "node:fs";

const candidates = [
  process.env.PLAYWRIGHT,
  "playwright",
  new URL("../../../../../orama-website-v2/node_modules/playwright/index.mjs", import.meta.url).href,
].filter(Boolean);
let chromium;
for (const spec of candidates) { try { ({ chromium } = await import(spec)); break; } catch {} }
if (!chromium) { console.error(`Playwright not found. Tried:\n  ${candidates.join("\n  ")}`); process.exit(1); }

const [base, ticketFile, out] = process.argv.slice(2);
const ticket = fs.readFileSync(ticketFile, "utf8").trim();
fs.mkdirSync(out, { recursive: true });
const log = [];
const note = (step, detail) => { log.push({ step, ...detail }); console.log(step, JSON.stringify(detail)); };

const browser = await chromium.launch();
const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
const page = await context.newPage();
let n = 0;
const shot = async (name) => page.screenshot({ path: `${out}/${String(++n).padStart(2, "0")}-${name}.png` });
const text = async () => (await page.locator("main").innerText()).replace(/\s+/g, " ").slice(0, 400);
const errorText = async () => (await page.locator("main .error").allInnerTexts()).join(" | ");

try {
  // 1. Sign in through Clerk's own form, which consumes the ticket, then lake's exchange.
  await page.goto(`${base}/sign-in?__clerk_ticket=${encodeURIComponent(ticket)}`);
  await page.waitForURL(`${base}/repositories**`, { timeout: 60000 });
  await shot("repositories");
  note("sign-in", { url: page.url(), you: (await page.content()).includes("(you)") || (await page.content()).includes("your personal space") });

  // 2. Design: the workspace gate, then wait for it to be ready.
  await page.goto(`${base}/design/projects`);
  await shot("projects-gate");
  if (await page.getByRole("button", { name: "Start your workspace" }).count()) {
    await page.getByRole("button", { name: "Start your workspace" }).click();
    await page.waitForLoadState("load");
    note("start-workspace", { text: await text() });
  }
  const deadline = Date.now() + 8 * 60 * 1000;
  while (Date.now() < deadline) {
    await page.goto(`${base}/design/projects`);
    if (await page.locator("form[action='/design/projects/create']").count()) break;
    if ((await text()).includes("could not be prepared")) break;
    await page.waitForTimeout(10000);
  }
  await shot("projects");
  note("workspace", { ready: await page.locator("form[action='/design/projects/create']").count() > 0, text: await text(), error: await errorText() });

  // 3. Create a project and build a pipeline from its intent.
  const project = `e2e-${Date.now().toString(36)}`;
  if (await page.locator("form[action='/design/projects/create']").count()) {
    await page.fill("input[name=name]", project);
    await page.fill("input[name=description]", "Which enterprise accounts will not renew?");
    await page.fill("textarea[name=intent]", "Which accounts the team calls first each week");
    await page.getByRole("button", { name: "Create project" }).click();
    await page.waitForLoadState("load");
    await shot("project");
    note("create-project", { url: page.url(), error: await errorText() });

    if (await page.locator("form[action='/design/project/compile']").count()) {
      await page.fill("form[action='/design/project/compile'] textarea[name=intent]",
        "Predict which accounts will churn in the next 90 days, with the reasons");
      await page.getByRole("button", { name: "Build pipeline" }).click();
      await page.waitForLoadState("load");
      await shot("pipeline");
      note("build-pipeline", { error: await errorText(), pipelines: await page.locator("form[action='/design/project/run'], form[action='/design/project/approve']").count() });
      // The pipeline page: draw it, add a step and an edge, then approve and check.
      const pipelineLink = page.locator("main table a[href*='/design/pipeline?']").first();
      if (await pipelineLink.count()) {
        await pipelineLink.click();
        await page.waitForLoadState("load");
        await shot("pipeline-page");
        note("pipeline-page", { svg: await page.locator("svg.pipeline-graph").count(), steps: await page.locator("svg.pipeline-graph g.step").count(), error: await errorText() });
        const addStep = page.locator("form[action='/design/pipeline/edit']:has(input[name=op][value=add_step])");
        await addStep.locator("select[name=kind]").selectOption("act");
        await addStep.locator("input[name=name]").fill("Notify the team");
        await addStep.getByRole("button", { name: "Add step" }).click();
        await page.waitForLoadState("load");
        note("pipeline-add-step", { url: page.url(), error: await errorText(), steps: await page.locator("svg.pipeline-graph g.step").count() });
        const connect = page.locator("form[action='/design/pipeline/edit']:has(input[name=op][value=add_edge]):has(input[name=from])").first();
        if (await connect.count()) {
          await connect.getByRole("button", { name: "Connect" }).click().catch(() => {});
          await page.waitForLoadState("load");
        }
        await shot("pipeline-edited");
        note("pipeline-edited", { error: await errorText(), edges: await page.locator("svg.pipeline-graph line.edge").count() });
        if (await page.locator("form[action='/design/pipeline/approve']").count()) {
          await page.getByRole("button", { name: "Approve" }).first().click();
          await page.waitForLoadState("load");
        }
        await page.locator("form[action='/design/pipeline/gate']").getByRole("button", { name: "Check" }).click();
        await page.waitForLoadState("load");
        await shot("pipeline-checked");
        note("pipeline-check", { checks: await page.locator("table.gate-checks tbody tr").count(), error: await errorText() });
        await page.goto(`${base}/design/project?name=${project}`);
      }
      if (await page.locator("form[action='/design/project/approve']").count()) {
        await page.getByRole("button", { name: "Approve" }).first().click();
        await page.waitForLoadState("load");
        await shot("approved");
        note("approve", { error: await errorText() });
      }
      if (await page.locator("form[action='/design/project/run']").count()) {
        await page.getByRole("button", { name: "Check and run" }).first().click();
        await page.waitForLoadState("load");
        await shot("run");
        note("check-and-run", { url: page.url(), error: await errorText() });
      }
    }
  }

  // 4. Every remaining page.
  for (const [path, name] of [["/train/data", "data"], ["/train/runs", "runs"], ["/ship/models", "models"],
                              ["/ship/indexes", "indexes"], ["/deployments", "deployments"], ["/services", "services"],
                              ["/", "home"]]) {
    const response = await page.goto(`${base}${path}`);
    await shot(name);
    note(`page ${path}`, { status: response.status(), error: await errorText(), text: (await text()).slice(0, 160) });
  }
  const runLink = await page.goto(`${base}/train/runs`).then(() => page.locator("a[href^='/train/run?']").first());
  if (await runLink.count()) {
    await runLink.click();
    await page.waitForLoadState("load");
    await shot("run-report");
    note("run-report", { url: page.url(), error: await errorText() });
  }

  // 4a. Search indexes (#187): upload a small CSV, index it, search, publish, call it from outside
  // with a key scoped to search_index, unpublish, and check the outside call is refused.
  {
    const csv = "id,title,body\n1,Renewal risk,Accounts that may not renew this quarter\n2,Expansion,Accounts ready to grow seats\n3,Support load,Tickets per account over time\n";
    const table = `e2e_docs_${Date.now().toString(36)}`;
    await page.goto(`${base}/train/data`);
    await page.locator("form[action='/train/data/upload']").evaluate((form) => { form.closest("details").open = true; });
    await page.setInputFiles("form[action='/train/data/upload'] input[type=file]", { name: "docs.csv", mimeType: "text/csv", buffer: Buffer.from(csv) });
    await page.fill("form[action='/train/data/upload'] input[name=table_name]", table);
    await page.getByRole("button", { name: "Upload" }).click();
    await page.waitForLoadState("load");
    note("upload-dataset", { error: await errorText(), listed: (await text()).length > 0 });
    await page.goto(`${base}/ship/indexes`);
    if (await page.locator("form[action='/ship/indexes/create']").count()) {
      const option = page.locator("form[action='/ship/indexes/create'] select[name=table] option", { hasText: table });
      await page.selectOption("form[action='/ship/indexes/create'] select[name=table]", await option.first().getAttribute("value") ?? table).catch(() => {});
      await page.getByRole("button", { name: "Create index" }).click();
      await page.waitForLoadState("load");
      const indexUrl = page.url();
      note("create-index", { url: indexUrl, error: await errorText() });
      await page.fill("form[action='/ship/index'] input[name=q]", "renewal");
      await page.getByRole("button", { name: "Search", exact: true }).click();
      await page.waitForLoadState("load");
      await shot("index-search");
      note("index-search", { hits: await page.locator("main table tbody tr").count(), error: await errorText() });
      await page.getByRole("button", { name: "Publish" }).click();
      await page.waitForLoadState("load");
      await page.getByRole("button", { name: "Create a search key" }).click();
      await page.waitForLoadState("load");
      const key = ((await page.locator("p.notice code").first().innerText().catch(() => "")) || "").trim();
      note("search-key", { issued: key.length > 20, url: page.url() });
      const id = new URL(indexUrl).searchParams.get("id");
      const customerId = new URL(indexUrl).searchParams.get("customer_id");
      const outside = async () => {
        const res = await fetch(`${base}/v1/search_index?customer_id=${customerId}&index=${id}&query=renewal`, { headers: { Authorization: `Bearer ${key}` } });
        const body = await res.json().catch(() => ({}));
        return { status: res.status, hits: body.value?.hits?.length ?? null, error: body.error?.message ?? "" };
      };
      note("outside-search", await outside());
      const other = await fetch(`${base}/v1/list_services?customer_id=${customerId}`, { headers: { Authorization: `Bearer ${key}` } });
      note("key-cannot-do-more", { status: other.status });
      await page.goto(indexUrl);
      await page.getByRole("button", { name: "Unpublish" }).click();
      await page.waitForLoadState("load");
      note("outside-search-after-unpublish", await outside());
    }
  }

  // 4b. Services: create a Postgres service, wait for Ready, show its connection, delete it.
  // First delete any e2e- service a crashed earlier run left behind.
  await page.goto(`${base}/services`);
  for (const leftover of await page.locator("tr:has(form[action='/services/delete'])").filter({ hasText: /^e2e-/ }).all()) {
    const leftoverName = (await leftover.locator("td").first().innerText()).trim().split(/\s/)[0];
    await leftover.locator("details summary").click();
    await leftover.locator("input[name=confirm]").fill(leftoverName);
    await leftover.getByRole("button", { name: `Delete ${leftoverName}` }).click();
    await page.waitForLoadState("load");
    note("cleanup-leftover-service", { name: leftoverName, error: await errorText() });
    await page.goto(`${base}/services`);
  }
  const service = `e2e-${Date.now().toString(36)}`;
  await page.goto(`${base}/services`);
  await page.selectOption("form[action='/services/create'] select[name=kind]", "postgres");
  await page.fill("form[action='/services/create'] input[name=name]", service);
  await page.fill("form[action='/services/create'] input[name=storage_gi]", "1");
  await page.getByRole("button", { name: "Create service" }).click();
  await page.waitForLoadState("load");
  note("create-service", { error: await errorText(), listed: (await text()).includes(service) });
  const row = () => page.locator("tr", { hasText: service });
  const serviceDeadline = Date.now() + 10 * 60 * 1000;
  while (Date.now() < serviceDeadline) {
    await page.goto(`${base}/services`);
    const status = (await row().count()) ? await row().locator("td").nth(3).innerText() : "";
    if (status.startsWith("Ready") || status.startsWith("Failed")) break;
    await page.waitForTimeout(10000);
  }
  await shot("service-ready");
  note("service-status", { status: (await row().count()) ? (await row().innerText()).replace(/\s+/g, " ") : "missing" });
  if (await row().locator("form[action='/services/connection']").count()) {
    await row().getByRole("button", { name: "Show connection" }).click();
    await page.waitForLoadState("load");
    await shot("service-connection");
    // The URI carries a password: record only that it was shown, never its value.
    note("service-connection", { shown: (await page.locator("main").innerText()).includes("postgresql://"), url: page.url() });
  }
  // 4c. Connect the Postgres service from Train → Data (#188) and pull a dataset through it.
  await page.goto(`${base}/services`);
  const serviceReady = (await row().count()) && (await row().locator("td").nth(3).innerText()).startsWith("Ready");
  if (serviceReady) {
    await page.goto(`${base}/train/data`);
    const connectRow = page.locator("tr", { hasText: service }).filter({ has: page.locator("form[action='/train/data/connect-service']") });
    if (await connectRow.count()) {
      await connectRow.getByRole("button").click();
      await page.waitForLoadState("load");
      note("connect-service", { error: await errorText(), listed: (await page.locator("main table", { hasText: service }).count()) > 0 });
      const pull = page.locator("form[action='/train/data/pull']");
      if (await pull.count()) {
        await pull.evaluate((form) => { form.closest("details").open = true; });
        await pull.locator("select[name=connection]").selectOption(service);
        await pull.locator("textarea[name=query]").fill("SELECT 1 AS one, 'lake' AS source");
        await pull.locator("input[name=table_name]").fill(`e2e_from_${Date.now().toString(36)}`);
        await page.getByRole("button", { name: "Pull dataset" }).click();
        await page.waitForLoadState("load");
        await shot("pulled-from-service");
        note("pull-from-service", { error: await errorText() });
      }
      const disconnect = page.locator("tr", { hasText: service }).locator("form[action='/train/data/disconnect']");
      if (await disconnect.count()) { await disconnect.getByRole("button").click(); await page.waitForLoadState("load"); }
    } else {
      note("connect-service", { error: "no Connect button for the service" });
    }
  }
  await page.goto(`${base}/services`);
  if (await row().count()) {
    await row().locator("details summary").click();
    await row().locator("input[name=confirm]").fill(service);
    await row().getByRole("button", { name: `Delete ${service}` }).click();
    await page.waitForLoadState("load");
    const deleteDeadline = Date.now() + 5 * 60 * 1000;
    while (Date.now() < deleteDeadline && (await row().count())) {
      await page.waitForTimeout(10000);
      await page.goto(`${base}/services`);
    }
    await shot("service-deleted");
    note("service-delete", { gone: (await row().count()) === 0, error: await errorText() });
  }

  // 4c. Delete this run's project and any earlier e2e- projects, once their runs have finished,
  // so the tester's workspace does not fill up (#164).
  await page.goto(`${base}/design/projects`);
  const projects = (await page.locator("main table a").allInnerTexts()).filter((name) => name.startsWith("e2e-"));
  for (const name of projects) {
    const cleanupDeadline = Date.now() + 5 * 60 * 1000;
    let error = "";
    while (Date.now() < cleanupDeadline) {
      await page.goto(`${base}/design/project?name=${encodeURIComponent(name)}`);
      if (!(await page.locator("form[action='/design/project/delete']").count())) break;
      await page.locator("form[action='/design/project/delete']").evaluate((form) => { form.closest("details").open = true; });
      await page.fill("form[action='/design/project/delete'] input[name=confirm]", name);
      await page.getByRole("button", { name: "Delete project" }).click();
      await page.waitForLoadState("load");
      error = await errorText();
      if (!error.includes("pending or running")) break;
      await page.waitForTimeout(15000);
    }
    note("delete-project", { name, error });
  }

  // 5. Account menu and sign out (signs out of Clerk too).
  await page.locator("details.account-menu summary").click();
  await shot("account-menu");
  await page.getByRole("button", { name: "Sign out" }).click();
  await page.waitForURL(`${base}/sign-in**`, { timeout: 30000 });
  await page.waitForTimeout(4000);
  await shot("signed-out");
  const after = await page.goto(`${base}/design/projects`);
  note("signed-out", { url: page.url(), status: after.status() });
} catch (error) {
  await shot("failure");
  note("failure", { message: String(error), url: page.url() });
  process.exitCode = 1;
} finally {
  fs.writeFileSync(`${out}/log.json`, JSON.stringify(log, null, 2));
  await browser.close();
}
