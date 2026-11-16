// SPDX-License-Identifier: MIT

// Change App end to end in a browser (lake#1259, tools/e2e/README.md): an App, then
// the app page's Change App split screen (lake#1241) and its conversation with the Application Agent
// (lake#1240): ask a small change, read the agent's proposal, Confirm, follow the redeploy, and check
// that the frame beside the conversation reloads onto the changed App.
//
//   node tools/e2e/change-app.mjs <base-url> <ticket-file> <output-dir> [app-id]
//
// Without an app id it makes one first, through the Application Agent's own propose-and-confirm
// turns (as agents.py T11 does): a small lake-authenticated page that says a fixed word. The change
// it asks for replaces that word with another, so the frame's text tells whether the change is what
// runs. Every /v1 answer that is not ok and every console error is written to log.json. It leaves
// the App in the account's workspace (named e2e-change-…). lib.mjs finds Playwright. Not C++: keep
// it out of git clang-format.
import { api, args, ensureFunds, ensureWorkspace, errorText, inWorkspace, loadChromium, logger, newPage, shooter, signIn, until } from "./lib.mjs";

const { base, ticketFile, out, rest } = args("node tools/e2e/change-app.mjs <base-url> <ticket-file> <output-dir> [app-id]");
const chromium = await loadChromium();
const log = logger(out);
const { note, check, failed } = log;
const { browser, page } = await newPage(chromium, base);
const shot = shooter(page, out);
const stamp = Date.now().toString(36);
const before = `before-${stamp}`;
const after = `after-${stamp}`;
// Text outside ASCII that the change must leave exactly as it is (lake#1238: files read as escaped
// text came back with literal \xNN in place of each byte).
const accented = "Café — déjà vu";

// Every failing /v1 answer and console error, for the log.
const failures = [];
page.on("console", (message) => {
  // Addresses without their query: the sign-in page's carries the one-time ticket.
  const bare = (url) => String(url ?? "").split("?")[0];
  if (message.type() === "error") failures.push({ console: message.text().slice(0, 300), resource: bare(message.location()?.url), page: bare(page.url()) });
});
page.on("response", async (response) => {
  const url = response.url();
  if (!url.includes("/v1/")) return;
  const body = await response.json().catch(() => null);
  if (body && body.ok === false) failures.push({ request: `${response.request().method()} ${url.replace(base, "").slice(0, 160)}`, status: response.status(), error: body.error });
});

/** One Application Agent turn through the API, followed until it finishes. Answers the task. */
async function turn(customer, workspace, input, minutes = 8) {
  const started = await api(page, base, "agent_turn", { customer_id: customer, workspace_id: workspace, agent: "agent-application", ...input }, { post: true, workspace });
  let task = {};
  await until(
    page,
    async () => {
      task = await api(page, base, "get_agent_task", { customer_id: customer, workspace_id: workspace, task_id: started.task_id }, { workspace });
      return task.status === "succeeded" || task.status === "failed";
    },
    { minutes, every: 4000 },
  );
  return { ...task, task_id: started.task_id };
}

/** A small App to change: proposed by the Application Agent, confirmed with the proposal's task and
 * digest (lake#561), and deployed. Answers its id. */
async function makeApp(customer, workspace) {
  const name = `e2e-change-${stamp}`;
  const proposed = await turn(customer, workspace, {
    kind: "app",
    message: `Build a lake-authenticated FastAPI app named ${name}. Its root URL answers an HTML page (UTF-8) whose body is exactly two lines: the word ${before}, then "${accented}". No data, no dependencies beyond FastAPI.`,
    context: {},
  });
  const proposal = proposed.answer?.result ?? {};
  if (!check("app proposed", proposed.status === "succeeded" && proposal.kind === "proposal", { task: proposed.task_id, status: proposed.status, message: proposed.message, kind: proposal.kind })) return "";
  const confirmed = await turn(
    customer,
    workspace,
    { kind: "app", message: "Confirm and deploy the reviewed proposal.", conversation_id: proposed.conversation_id, proposal_task_id: proposed.task_id, proposal_sha256: proposed.proposal_sha256 ?? "", context: { confirmed_proposal: proposal } },
    12,
  );
  const deployed = confirmed.answer?.result ?? {};
  const appId = deployed.app?.id ?? "";
  check("app deployed", confirmed.status === "succeeded" && deployed.kind === "deployed" && !!appId, { task: confirmed.task_id, status: confirmed.status, message: confirmed.message, kind: deployed.kind, app: appId });
  return appId;
}

/** The text the running App's frame shows, or "". */
async function frameText() {
  const frame = page.frameLocator("iframe[data-reloads]");
  return (await frame.locator("body").innerText({ timeout: 15000 }).catch(() => "")).trim();
}

try {
  const viewer = await signIn(page, base, ticketFile);
  note("signed in", { customer: viewer.customer_id });
  await ensureFunds(page, base, log);
  const workspace = await ensureWorkspace(page, base, log);
  if (!workspace) throw new Error("no workspace");
  const customer = viewer.customer_id;
  const appId = rest[0] || (await makeApp(customer, workspace));
  if (!appId) throw new Error("no app to change");

  // The App answers before the change.
  let app = {};
  const ready = await until(page, async () => {
    app = await api(page, base, "get_app", { customer_id: customer, workspace_id: workspace, app_id: appId }, { workspace });
    return app.ready === true && typeof app.url === "string" && app.url.startsWith("http");
  }, { minutes: 5 });
  check("app ready", ready, { app: appId, phase: app.phase, url: app.url, generation: app.generation });

  // The app page → Change App opens the split screen with the App in its frame.
  await page.goto(inWorkspace(base, workspace, `/ship/app?customer_id=${encodeURIComponent(customer)}&app_id=${encodeURIComponent(appId)}`));
  const changeButton = page.getByRole("button", { name: "Change App" });
  await changeButton.waitFor({ timeout: 30000 });
  await until(page, async () => changeButton.isEnabled(), { minutes: 1, every: 1000 });
  await changeButton.click();
  const split = page.getByRole("dialog", { name: /^Change / });
  await split.waitFor({ timeout: 15000 });
  const frame = split.locator("iframe[data-reloads]");
  await frame.waitFor({ timeout: 15000 }).catch(() => {});
  const first = await frameText();
  await shot("split-open");
  check("split screen", (await frame.count()) > 0 && new URL(page.url()).searchParams.get("change") === "1", { frame_text: first.slice(0, 200), error: await errorText(page) });
  const word = rest[0] ? first.split(/\s+/).find((w) => /^[a-z]+-[a-z0-9]+$/.test(w)) : before;
  check("frame shows the App", !!word && first.includes(word), { frame_text: first.slice(0, 200), want: word });
  const keepsAccents = first.includes(accented);

  // Ask a small change; the agent proposes a diff.
  const message = split.getByRole("textbox", { name: "Message to the Application Agent" });
  await message.fill(`Change the word ${word} on the page to ${after}. Change nothing else.`);
  await split.getByRole("button", { name: "Send" }).click();
  const proposal = split.locator('[data-role="agent"][data-state="proposal"]');
  const settled = split.locator('[data-role="agent"]:is([data-state="proposal"],[data-state="failed"],[data-state="question"],[data-state="reply"])');
  await settled.last().waitFor({ timeout: 8 * 60000 }).catch(() => {});
  const conversation = await api(page, base, "get_app_conversation", { customer_id: customer, workspace_id: workspace, app_id: appId }, { workspace }).catch((e) => ({ error: e.message }));
  const asked = (conversation.turns ?? []).at(-1) ?? {};
  await shot("proposal");
  const proposed = check("agent proposes", (await proposal.count()) > 0, {
    task: asked.task_id,
    status: asked.status,
    state: await settled.last().getAttribute("data-state").catch(() => null),
    reply: (await settled.last().innerText().catch(() => "")).slice(0, 600),
    message: asked.message,
    error: conversation.error,
  });

  if (proposed) {
    // Confirm: the agent commits the change and redeploys; the frame reloads once it is Ready.
    const reloads = Number((await frame.getAttribute("data-reloads")) ?? 0);
    await split.getByRole("button", { name: "Confirm and apply the change" }).click();
    const applied = split.locator('[data-role="agent"]:is([data-state="applied"],[data-state="failed"])');
    await applied.last().waitFor({ timeout: 12 * 60000 }).catch(() => {});
    const state = await applied.last().getAttribute("data-state").catch(() => null);
    const latest = await api(page, base, "get_app_conversation", { customer_id: customer, workspace_id: workspace, app_id: appId }, { workspace }).catch((e) => ({ error: e.message }));
    const confirmTurn = (latest.turns ?? []).at(-1) ?? {};
    await shot("applied");
    check("change applied", state === "applied", {
      task: confirmTurn.task_id,
      status: confirmTurn.status,
      confirmation: confirmTurn.confirmation,
      kind: confirmTurn.answer?.result?.kind,
      reply: (await applied.last().innerText().catch(() => "")).slice(0, 600),
      message: confirmTurn.message,
      error: (await errorText(page)) || latest.error,
    });

    // The frame reloads by itself onto the changed App, which says the new word.
    const reloaded = await until(page, async () => Number((await frame.getAttribute("data-reloads")) ?? 0) > reloads && (await frameText()).includes(after), { minutes: 10, every: 3000 });
    const shown = await frameText();
    app = await api(page, base, "get_app", { customer_id: customer, workspace_id: workspace, app_id: appId }, { workspace }).catch(() => ({}));
    await shot("reloaded");
    check("frame reloads onto the change", reloaded, {
      reloads: Number((await frame.getAttribute("data-reloads")) ?? 0),
      frame_text: shown.slice(0, 200),
      phase: app.phase,
      generation: app.generation,
      observed_generation: app.observed_generation,
      banner: (await split.getByRole("status").allInnerTexts().catch(() => [])).join(" | "),
    });
    // Text outside ASCII is the App's own after the change, not \xNN escapes (lake#1238).
    if (!reloaded) note("the frame never showed the change; the check of text outside ASCII is skipped", {});
    else if (keepsAccents) check("the change keeps text outside ASCII", shown.includes(accented) && !/\\x[0-9a-f]{2}/i.test(shown), { frame_text: shown.slice(0, 200), want: accented });
    else note("the App did not show the accented line before the change; the check is skipped", { frame_text: first.slice(0, 200) });
  }
  await split.getByRole("button", { name: "Close Change App" }).click();
} catch (error) {
  await shot("failure");
  check("script", false, { error: String(error?.message ?? error).slice(0, 500) });
} finally {
  note("failed requests and console errors", { count: failures.length, failures: failures.slice(0, 40) });
  await browser.close();
}
process.exit(failed() ? 1 : 0);
