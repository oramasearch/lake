// End-to-end browser walkthrough of lake's console (docs/deployment/browser-testing.md): Clerk
// sign-in with a one-time ticket, landing on `/` (Quickstart), then Design, Train and Ship as a
// person does them, with a screenshot per step, a log.json, and pass/fail checks (exit 1 on any
// failure).
//
//   node tools/e2e/walkthrough.mjs <base-url> <ticket-file> <output-dir>
//
// A new account is blocked until its first top-up: the walkthrough adds the first $20 on Stripe's
// test Checkout (refused unless the session is test mode) and starts the first workspace from Ask
// Orama's home. It creates one project, one dataset, one search index and one Postgres service in
// the signed-in person's workspace, and deletes the project, the service and any e2e- project or
// service an earlier run left behind. lib.mjs finds Playwright. Not C++: keep it out of git
// clang-format.
import { api, args, ensureFunds, ensureWorkspace, errorText, inWorkspace, loadChromium, logger, newPage, shooter, signIn, until } from "./lib.mjs";

const { base, ticketFile, out } = args("node tools/e2e/walkthrough.mjs <base-url> <ticket-file> <output-dir>");
const chromium = await loadChromium();
const log = logger(out);
const { note, check, failed } = log;
const { browser, page } = await newPage(chromium, base);
const shot = shooter(page, out);
const main = () => page.locator("main");
const dialog = () => page.getByRole("dialog");
const h1 = async () => (await main().getByRole("heading", { level: 1 }).first().innerText().catch(() => "")).trim();
const settle = () => page.waitForLoadState("networkidle", { timeout: 15000 }).catch(() => {});
let viewer;
let workspace = "";
const ws = (path) => inWorkspace(base, workspace, path);
const stamp = Date.now().toString(36);

/** Design → Projects → New project, then Build pipeline from its intent; the pipeline page: the
 * graph, a step and an edge added, Approve and Check; then Check and run from the project. */
async function design() {
  const project = `e2e-${stamp}`;
  await page.goto(`${base}/design/projects`);
  await main().getByRole("heading", { level: 1, name: "Projects" }).waitFor();
  await shot("projects");
  check("projects page", page.url().startsWith(ws("/design/projects")), { url: page.url() });

  await main().getByRole("button", { name: "New project" }).first().click();
  await dialog().getByRole("textbox", { name: "Name" }).fill(project);
  await dialog().getByRole("textbox", { name: "Description" }).fill("Which enterprise accounts will not renew?");
  await dialog().getByRole("textbox", { name: "The decision it should improve" }).fill("Which accounts the team calls first each week");
  await dialog().getByRole("button", { name: "Create project" }).click();
  await page.waitForURL(/\/design\/project\?/, { timeout: 60000 }).catch(() => {});
  await main().getByRole("heading", { level: 1, name: new RegExp(project) }).waitFor({ timeout: 30000 }).catch(() => {});
  await shot("project");
  if (!check("create project", (await h1()).includes(project), { url: page.url(), error: await errorText(page) })) return project;

  // Build a pipeline from what it should predict; with no dataset chosen the drawer closes on the
  // project and the pipeline is listed.
  await main().getByRole("button", { name: "Build pipeline" }).first().click();
  await dialog().getByRole("textbox", { name: "What should it predict or answer?" }).fill("Predict which accounts will churn in the next 90 days, with the reasons");
  await dialog().getByRole("button", { name: "Build pipeline" }).click();
  await dialog().waitFor({ state: "detached", timeout: 120000 }).catch(() => {});
  const drawerError = (await dialog().count()) ? (await dialog().getByRole("alert").allInnerTexts()).join(" | ") : "";
  if (await dialog().count()) await dialog().getByRole("button", { name: "Cancel" }).click();
  await page.goto(ws(`/design/project?name=${encodeURIComponent(project)}&tab=pipelines`));
  const pipelines = main().getByRole("table", { name: "Pipelines" });
  const pipelineLink = pipelines.getByRole("link").first();
  await pipelineLink.waitFor({ timeout: 60000 }).catch(() => {});
  await shot("pipeline-built");
  if (!check("build pipeline", (await pipelineLink.count()) > 0, { error: drawerError || (await errorText(page)) })) return project;

  // The pipeline page: the graph is drawn; add a step, connect it, approve, check.
  await pipelineLink.click();
  await page.waitForURL(/\/design\/pipeline\?/);
  const graphPanel = main().locator(".pipeline-canvas svg").first();
  await graphPanel.waitFor({ timeout: 30000 }).catch(() => {});
  const counts = async () => {
    // The graph panel's label; textContent, since the label is shown in capitals.
    const label = await main().getByText(/^\d+ steps · \d+ edges$/i).first().textContent().catch(() => "");
    const [, steps, edges] = (label ?? "").match(/(\d+) steps · (\d+) edges/i) ?? [];
    return { steps: Number(steps ?? -1), edges: Number(edges ?? -1) };
  };
  const before = await counts();
  await shot("pipeline-page");
  check("pipeline page", (await graphPanel.count()) > 0 && before.steps > 0, { ...before, error: await errorText(page) });

  const change = main().getByTestId("change-pipeline").or(main().locator("section").filter({ hasText: /^Change the Pipeline/ })).first();
  await change.getByRole("combobox", { name: "Kind" }).selectOption("act");
  await change.getByRole("textbox", { name: "Name" }).first().fill("Notify the team");
  await change.getByRole("button", { name: "Add step" }).click();
  await until(page, async () => (await counts()).steps === before.steps + 1, { minutes: 1, every: 1000 });
  const step = new URL(page.url()).searchParams.get("step") ?? "";
  const added = await counts();
  check("pipeline add step", added.steps === before.steps + 1 && !!step, { step, ...added, error: await errorText(page) });

  // Connect the step that came before it to the new step.
  const options = await change.getByRole("combobox", { name: "From" }).evaluate((select) => [...select.options].map((o) => o.value));
  const from = options.filter((value) => value !== step).at(-1);
  if (from && step) {
    await change.getByRole("combobox", { name: "From" }).selectOption(from);
    await change.getByRole("combobox", { name: "To" }).selectOption(step);
    await change.getByRole("button", { name: "Add edge" }).click();
    await until(page, async () => (await counts()).edges === added.edges + 1, { minutes: 1, every: 1000 });
  }
  const edited = await counts();
  await shot("pipeline-edited");
  check("pipeline add edge", edited.edges === added.edges + 1, { from, to: step, ...edited, error: await errorText(page) });

  const govern = main().locator("#run");
  await govern.getByRole("button", { name: "Approve" }).click();
  await govern.getByRole("button", { name: "Approved" }).waitFor({ timeout: 30000 }).catch(() => {});
  check("approve", (await govern.getByRole("button", { name: "Approved" }).count()) > 0, { error: await errorText(page) });
  await govern.getByRole("button", { name: "Check", exact: true }).click();
  const checks = main().getByRole("table", { name: "Checks" });
  await checks.waitFor({ timeout: 60000 }).catch(() => {});
  await shot("pipeline-checked");
  check("pipeline check", (await checks.count()) > 0, { checks: Math.max(0, (await checks.getByRole("row").count()) - 1), error: await errorText(page) });

  // From the project: Check and run the approved pipeline; a run that starts opens Train → Runs.
  await page.goto(ws(`/design/project?name=${encodeURIComponent(project)}&tab=pipelines`));
  const run = main().getByRole("table", { name: "Pipelines" }).getByRole("button", { name: "Check and run" }).first();
  await run.waitFor({ timeout: 30000 }).catch(() => {});
  if (await run.count()) {
    await run.click();
    await page.waitForURL(/\/train\/runs/, { timeout: 60000 }).catch(() => {});
    await settle();
    await shot("run");
    check("check and run", /\/train\/runs/.test(page.url()), { url: page.url(), error: await errorText(page) });
  } else {
    check("check and run", false, { error: "no Check and run button for the approved pipeline" });
  }
  return project;
}

/** Every remaining page opens with its title and no error. */
async function pages() {
  for (const [path, title] of [["/train/data", "Datasets"], ["/train/runs", "Runs"], ["/ship/models", "Models"], ["/ship/indexes", "Search Indexes"],
                               ["/deployments", "Deployments"], ["/services", "Services"],
                               ["/", "Search and Model Answers on Your Website"], ["/ask", /^Hello/]]) {
    await page.goto(`${base}${path}`);
    await main().getByRole("heading", { level: 1, name: title }).waitFor({ timeout: 30000 }).catch(() => {});
    await settle();
    await shot(path === "/" ? "home" : path.split("/").pop());
    const heading = await h1();
    const error = await errorText(page);
    check(`page ${path}`, (typeof title === "string" ? heading === title : title.test(heading)) && !error, { url: page.url(), heading, error, text: (await main().innerText()).replace(/\s+/g, " ").slice(0, 160) });
  }
  // The first run's report.
  await page.goto(`${base}/train/runs`);
  const report = main().getByRole("link", { name: /^(Report|Why it failed)$/ }).first();
  await report.waitFor({ timeout: 20000 }).catch(() => {});
  if (await report.count()) {
    await report.click();
    await page.waitForURL(/\/train\/run\?/);
    await settle();
    await shot("run-report");
    check("run report", !!(await h1()), { url: page.url(), heading: await h1(), error: await errorText(page) });
  } else {
    note("run report", { skipped: "no run listed" });
  }
}

/** Search indexes (#187): upload a small CSV on Data, index it, search, publish, call it from
 * outside with a key that can only search, unpublish, and check the outside call is refused. */
async function searchIndex() {
  const csv = "id,title,body\n1,Renewal risk,Accounts that may not renew this quarter\n2,Expansion,Accounts ready to grow seats\n3,Support load,Tickets per account over time\n";
  const table = `e2e_docs_${stamp}`;
  await page.goto(ws("/train/data"));
  await main().getByRole("button", { name: "Load data" }).first().click();
  await dialog().locator("input[type=file]").setInputFiles({ name: "docs.csv", mimeType: "text/csv", buffer: Buffer.from(csv) });
  await dialog().getByRole("textbox", { name: /^Dataset name/ }).fill(table);
  await dialog().getByRole("button", { name: "Upload", exact: true }).last().click();
  await dialog().waitFor({ state: "detached", timeout: 60000 }).catch(() => {});
  await main().getByRole("region", { name: "Datasets" }).getByText(table).first().waitFor({ timeout: 30000 }).catch(() => {});
  await shot("dataset-uploaded");
  if (!check("upload dataset", (await main().getByText(table).count()) > 0, { table, error: await errorText(page) })) return;

  await page.goto(ws("/ship/indexes"));
  await main().getByRole("button", { name: "New index" }).first().click();
  await dialog().getByRole("combobox", { name: "Dataset" }).selectOption({ label: table });
  await dialog().getByRole("button", { name: /^Create/ }).click();
  await page.waitForURL(/\/ship\/index\?/, { timeout: 60000 }).catch(() => {});
  const indexUrl = page.url();
  const id = new URL(indexUrl).searchParams.get("id") ?? "";
  if (!check("create index", /\/ship\/index\?/.test(indexUrl) && !!id, { url: indexUrl, error: await errorText(page) })) return;

  await main().getByRole("textbox", { name: "Search for" }).fill("renewal");
  await main().getByRole("search").getByRole("button", { name: "Search" }).click();
  const hits = main().getByRole("list", { name: "Search hits" }).getByRole("listitem");
  await hits.first().waitFor({ timeout: 30000 }).catch(() => {});
  await shot("index-search");
  check("index search", (await hits.count()) > 0, { hits: await hits.count(), error: await errorText(page) });

  await main().getByRole("button", { name: "Publish", exact: true }).click();
  await main().getByRole("button", { name: "Create a search key" }).click();
  const keyBlock = main().locator(".copy-block").filter({ hasText: "Your search key" }).locator("code");
  await keyBlock.waitFor({ timeout: 30000 }).catch(() => {});
  const key = ((await keyBlock.innerText().catch(() => "")) || "").trim();
  check("search key", key.length > 20, { length: key.length });
  const outside = async () => {
    const url = `${base}/v1/search_index?customer_id=${viewer.customer_id}&workspace_id=${workspace}&index_id=${encodeURIComponent(id)}&query=renewal`;
    const res = await fetch(url, { headers: { Authorization: `Bearer ${key}` } });
    const body = await res.json().catch(() => ({}));
    return { status: res.status, hits: body.value?.hits?.length ?? null, error: body.error?.message ?? "" };
  };
  const published = await outside();
  check("outside search", published.status === 200 && published.hits > 0, published);
  const other = await fetch(`${base}/v1/list_services?customer_id=${viewer.customer_id}`, { headers: { Authorization: `Bearer ${key}` } });
  check("key cannot do more", other.status === 401 || other.status === 403, { status: other.status });

  await page.goto(indexUrl);
  await main().getByRole("button", { name: "Unpublish", exact: true }).click();
  await dialog().getByRole("button", { name: "Unpublish", exact: true }).click();
  await main().getByRole("button", { name: "Publish", exact: true }).waitFor({ timeout: 30000 }).catch(() => {});
  const after = await outside();
  check("outside search after unpublish", after.status !== 200, after);
  // The key is revoked again on Tokens (it is named "search <table>").
  await page.goto(`${base}/tokens`);
  const row = main().getByRole("row").filter({ hasText: `search ${table}` });
  if (await row.count()) {
    await row.getByRole("button", { name: "Revoke" }).click();
    const confirm = dialog().getByRole("button", { name: "Revoke" });
    if (await confirm.count()) await confirm.click();
  }
}

/** A Postgres service: create, Ready, Show connection, connect it from Data → Connections and pull
 * a dataset through it (#188), then delete it. Leftover e2e- services go first. */
async function services() {
  const table = () => main().getByRole("table", { name: "Services" });
  const statusOf = async (row) =>
    (await row.getByTestId("service-status").or(row.getByRole("cell").nth(3)).first().innerText().catch(() => "")).replace(/\s+/g, " ").replace(/^[^A-Za-z]+/, "");
  const remove = async (name) => {
    await page.goto(`${base}/services`);
    await table().waitFor({ timeout: 30000 });
    const row = table().getByRole("row").filter({ hasText: name });
    if (!(await row.count())) return true;
    await row.getByRole("button", { name: `Delete ${name}` }).click();
    await dialog().getByRole("textbox").fill(name);
    await dialog().getByRole("button", { name: `Delete ${name}` }).click();
    // Gone means the table is listed without it (not an error panel in its place).
    return until(page, async () => (await table().count()) > 0 && (await table().getByRole("row").filter({ hasText: name }).count()) === 0, { minutes: 5, every: 3000 });
  };
  await page.goto(`${base}/services`);
  await table().waitFor({ timeout: 30000 }).catch(() => {});
  for (const leftover of (await table().locator("strong").allInnerTexts()).filter((name) => name.startsWith("e2e-"))) {
    note("cleanup-leftover-service", { name: leftover, gone: await remove(leftover) });
  }

  const service = `e2e-${stamp}`;
  await page.goto(`${base}/services`);
  await main().getByRole("button", { name: "New service" }).first().click();
  await dialog().getByRole("combobox", { name: "Kind" }).selectOption("postgres");
  await dialog().getByRole("textbox", { name: "Name" }).fill(service);
  await dialog().getByRole("spinbutton", { name: "Storage (GiB)" }).fill("1");
  await dialog().getByRole("button", { name: "Create service" }).click();
  await dialog().waitFor({ state: "detached", timeout: 30000 }).catch(() => {});
  const row = () => table().getByRole("row").filter({ hasText: service });
  const listed = await until(page, async () => (await row().count()) > 0, { minutes: 1, every: 1000 });
  check("create service", listed, { error: (await dialog().count()) ? await dialog().innerText() : await errorText(page) });
  let status = "";
  await until(page, async () => {
    status = (await row().count()) ? await statusOf(row()) : "";
    return /^(Ready|Failed)/.test(status);
  }, { minutes: 10, every: 3000 });
  await shot("service-ready");
  const ready = check("service ready", status.startsWith("Ready"), { status });

  if (ready) {
    await row().getByRole("button", { name: "Show connection" }).click();
    await dialog().waitFor();
    await shot("service-connection");
    // The URI carries a password: record only that it was shown, never its value.
    check("service connection", (await dialog().locator("dl dt").allInnerTexts()).includes("uri") && (await dialog().innerText()).includes("postgresql://"), {});
    await dialog().getByRole("button", { name: "Close" }).first().click();

    // Data → Connections: Connect One of Your Services, then Load data → From a connection.
    await page.goto(ws("/connections"));
    const item = main().getByRole("listitem").filter({ hasText: service });
    await item.getByRole("button", { name: /^(Connect|Refresh)$/ }).click();
    const connected = await until(page, async () => (await main().getByRole("table").filter({ hasText: service }).count()) > 0, { minutes: 1, every: 1000 });
    await shot("service-connected");
    check("connect service", connected, { error: await errorText(page) });
    if (connected) {
      await page.goto(ws("/train/data"));
      await main().getByRole("button", { name: "Load data" }).first().click();
      await dialog().getByRole("button", { name: "From a connection" }).click();
      await dialog().getByRole("combobox", { name: /Connection/ }).selectOption(service).catch(() => {});
      await dialog().getByRole("textbox", { name: /Query|SELECT/ }).first().fill("SELECT 1 AS one, 'lake' AS source");
      await dialog().getByRole("textbox", { name: /Dataset name|Table name/ }).first().fill(`e2e_from_${stamp}`);
      await dialog().getByRole("button", { name: "Pull dataset" }).click();
      await dialog().waitFor({ state: "detached", timeout: 60000 }).catch(() => {});
      const pullError = (await dialog().count()) ? (await dialog().getByRole("alert").allInnerTexts()).join(" | ") : await errorText(page);
      await shot("pulled-from-service");
      check("pull from service", !(await dialog().count()), { error: pullError.slice(0, 300) });
      if (await dialog().count()) await dialog().getByRole("button", { name: "Cancel" }).click();
      await api(page, base, "workspace_act", { customer_id: viewer.customer_id, workspace_id: workspace, capability: "delete_connection", input: { name: service } }, { post: true, workspace })
        .catch((error) => note("disconnect", { error: error.message }));
    }
  }
  check("service delete", await remove(service), {});
  await shot("service-deleted");
}

/** Delete this run's project and any earlier e2e- project, once its runs have finished (#164). */
async function deleteProjects(project) {
  await page.goto(ws("/design/projects"));
  // The list arrives after the page: wait for this run's own project to be listed.
  await main().getByRole("link", { name: project, exact: true }).waitFor({ timeout: 30000 }).catch(() => {});
  const names = (await main().getByRole("link").allInnerTexts()).map((name) => name.trim()).filter((name) => /^e2e-[a-z0-9]+$/.test(name));
  check("projects to delete", names.includes(project), { names });
  for (const name of [...new Set(names)]) {
    let error = "";
    const deadline = Date.now() + 5 * 60 * 1000;
    while (Date.now() < deadline) {
      await page.goto(ws(`/design/project?name=${encodeURIComponent(name)}`));
      const button = main().getByRole("button", { name: "Delete project" });
      await button.waitFor({ timeout: 20000 }).catch(() => {});
      if (!(await button.count())) { error = "no Delete project button"; break; }
      await button.click();
      await dialog().getByRole("textbox").fill(name);
      await dialog().getByRole("button", { name: "Delete project" }).click();
      await page.waitForURL(/\/design\/projects/, { timeout: 30000 }).catch(() => {});
      error = (await dialog().count()) ? (await dialog().getByRole("alert").allInnerTexts()).join(" | ") : "";
      if (!error.includes("pending or running")) break;
      await dialog().getByRole("button", { name: "Cancel" }).click();
      await page.waitForTimeout(15000);
    }
    check(`delete project ${name}`, !error, { error });
  }
}

try {
  // 1. Sign in through Clerk's own form; lake lands a signed-in person on `/` (Quickstart).
  viewer = await signIn(page, base, ticketFile);
  await shot("signed-in");
  check("sign-in", new URL(page.url()).pathname === "/", { url: page.url(), heading: await h1() });

  // 2. A new account's first top-up and first workspace.
  await ensureFunds(page, base, log);
  workspace = await ensureWorkspace(page, base, log);
  if (!workspace) throw new Error("no ready workspace");

  // 3. Design, Train and Ship.
  const project = await design();
  await pages();
  await searchIndex();
  await services();
  await deleteProjects(project);

  // 4. The account menu at the foot of the rail, then Sign out (lake and Clerk both sign out).
  await page.getByRole("navigation", { name: "Navigation" }).locator("button[aria-haspopup=menu]").click();
  await shot("account-menu");
  await page.getByRole("menuitem", { name: "Sign out" }).click();
  await page.waitForURL(`${base}/sign-in**`, { timeout: 30000 });
  await page.waitForTimeout(4000);
  await shot("signed-out");
  await page.goto(`${base}/design/projects`);
  await page.waitForURL(`${base}/sign-in**`, { timeout: 30000 }).catch(() => {});
  const signedIn = await api(page, base, "viewer").then((v) => v.signed_in).catch(() => false);
  check("signed out", !signedIn && /\/sign-in/.test(page.url()), { url: page.url() });
} catch (error) {
  await shot("failure");
  check("failure", false, { message: String(error).slice(0, 400), url: page.url() });
} finally {
  await browser.close();
  process.exit(failed() ? 1 : 0);
}
