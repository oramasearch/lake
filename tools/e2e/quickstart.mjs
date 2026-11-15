// SPDX-License-Identifier: MIT

// Browser walkthrough of the quickstart process (lake#1128, lake#1182, docs/ui/quickstart.md): Clerk
// sign-in with a one-time ticket, which lands on Quickstart at `/`; the first top-up when the
// account's prepaid balance is blocked (lib.ensureFunds, Stripe's test Checkout only); then a CSV
// dropped on the page, the live search, a website key and the plain web page code, which it serves
// from another origin and searches there; then step 4, the Ask Orama conversation about the
// dataset: it waits for Ask Orama's proposals, asks for a model when none is proposed, confirms the
// train_quickstart proposal in the chat, checks the page moves to training at once and the chat says
// what started (lake#1204), and follows training and publishing to the model's answers on the page
// and on the website. Then a failure: a one-row dataset asked for clustering, whose page must show
// the reason in plain words with Try again and Ask Orama to fix it. A screenshot per step and a log.json. It leaves the dataset, index,
// model, key and conversation in the signed-in account's workspace; delete them from Data, Ship
// (the index's Website Keys) and Ask Orama after a run.
//
//   node tools/e2e/quickstart.mjs <base-url> <ticket-file> <output-dir>
//
// Playwright is found as lib.mjs finds it (PLAYWRIGHT, a `playwright` package, or the sibling
// orama-website-v2 checkout). Not C++: keep it out of git clang-format.
import fs from "node:fs";
import http from "node:http";
import { args, ensureFunds, errorText, loadChromium, logger, newPage, shooter, signIn, until } from "./lib.mjs";

const { base, ticketFile, out } = args("quickstart.mjs <base-url> <ticket-file> <output-dir>");
const chromium = await loadChromium();
const { note, check, failed } = logger(out);

// A small dataset with a text column to search and an outcome to learn.
const films = [
  ["The Matrix", "A hacker learns reality is a simulation", "scifi", 8.7, 1],
  ["Inception", "A thief steals secrets through dreams", "scifi", 8.8, 1],
  ["Amelie", "A shy waitress helps people in Paris", "romance", 8.3, 0],
  ["Alien", "A crew meets a deadly creature in space", "horror", 8.5, 1],
  ["Notting Hill", "A bookseller falls for a film star", "romance", 7.2, 0],
  ["Heat", "A detective hunts a crew of thieves", "crime", 8.3, 1],
  ["Clue", "Guests at a mansion solve a murder", "comedy", 7.2, 0],
  ["Arrival", "A linguist talks with visitors from space", "scifi", 7.9, 1],
  ["Big", "A boy wakes up as an adult", "comedy", 7.3, 0],
  ["Ronin", "Mercenaries chase a mysterious case", "crime", 7.2, 0],
];
const rows = ["title,description,genre,rating,popular"];
for (let copy = 0; copy < 4; copy++) for (const [t, d, g, r, p] of films) rows.push(`${t} ${copy},${d},${g},${r},${p}`);
const csv = `${out}/e2e_quickstart_${Date.now().toString(36)}.csv`;
fs.writeFileSync(csv, rows.join("\n") + "\n");

// The website: a page on another origin that holds whatever code the console gave.
let sitePage = "<!doctype html><title>site</title>";
const site = http.createServer((_, res) => {
  res.writeHead(200, { "Content-Type": "text/html; charset=utf-8" });
  res.end(sitePage);
});
await new Promise((resolve) => site.listen(0, "127.0.0.1", resolve));
const siteUrl = `http://127.0.0.1:${site.address().port}/`;

const { browser, context, page } = await newPage(chromium, base);
const shot = shooter(page, out);
const shotOf = (other, name) => other.screenshot({ path: `${out}/${name}.png`, fullPage: true }).catch(() => {});
const stepState = (step) => page.getByTestId(`quickstart-step-${step}`).getAttribute("data-state");
const visible = async (id) => (await page.getByTestId(id).count()) > 0;

/** The Quickstart page has read its status: the drop zone or a quickstart's steps are on it. */
const settled = () => until(page, async () => (await visible("quickstart-drop")) || (await visible("quickstart-start-another")), { minutes: 2, every: 1000 });

/** Attach the CSV to the drop zone that is on the page now, and check the page took it (the file's
 * name shown, the start button enabled). The drop zone can be replaced while the page reads its
 * status, which drops a file attached to the old one, so it is attached again until it holds. */
async function dropFile() {
  for (let attempt = 1; attempt <= 5; attempt++) {
    const drop = page.getByTestId("quickstart-drop");
    await drop.waitFor({ timeout: 60000 });
    await drop.locator("input[type=file]").setInputFiles(csv);
    const held = await until(page, async () => (await drop.locator("label[data-chosen=true]").count()) > 0 && (await page.getByTestId("quickstart-start").isEnabled()), { minutes: 0.25, every: 500 });
    if (held) return attempt;
    await page.waitForTimeout(2000);
  }
  return 0;
}

/** The website step's plain web page code: the Source code view, then its snippet. */
async function websiteCode() {
  const view = page.getByRole("button", { name: "Source code" });
  if (await view.count()) await view.click();
  await page.getByRole("button", { name: "Plain web page" }).click().catch(() => {});
  return page.getByTestId("quickstart-snippet-html").locator("code").innerText();
}

/** The train_quickstart proposal in the conversation, with its Confirm button, or null. */
async function modelProposal() {
  const proposal = page.getByTestId("quickstart-conversation").locator(".ask-proposal", { hasText: "train_quickstart" }).last();
  if (!(await proposal.count())) return null;
  const confirm = proposal.getByRole("button", { name: /^(Confirm|Try again)$/ });
  return (await confirm.count()) ? { proposal, confirm } : null;
}

/** A failed model step on the page (lake#1204): the reason in plain words (no engine internals)
 * and its two next actions, Try again and Ask Orama to fix it. */
async function failureShown(name) {
  const reason = await page.getByTestId("quickstart-model-problem").innerText().catch(() => "");
  const plain = !!reason && !/configure|graph hints|K range|segmentation|holdout|train_model|train_quickstart/i.test(reason);
  const actions = (await visible("quickstart-try-again")) && (await visible("quickstart-ask-fix"));
  await shot(name);
  check(name, plain && actions, { reason, actions });
}

/** Ask Orama's turn is over: no working notice in the conversation, and it has answered. */
const answered = async () => (await page.getByTestId("quickstart-conversation").locator(".ask-working").count()) === 0 && (await page.getByTestId("quickstart-conversation").locator(".ask-agent").count()) > 0;

try {
  // 1. Sign in (an account lands on Quickstart at `/`), then the first top-up when blocked.
  const viewer = await signIn(page, base, ticketFile);
  note("sign-in", { url: page.url(), customer: viewer.customer_id });
  if (!(await ensureFunds(page, base, { note, check }))) throw new Error("the account has no funds and the top-up did not complete");
  await page.goto(`${base}/`, { waitUntil: "domcontentloaded" });
  await page.getByTestId("quickstart-page").waitFor({ timeout: 60000 });
  if (!(await settled())) throw new Error("the Quickstart page did not read its status");
  await shot("quickstart-home");
  if (await visible("quickstart-start-another")) await page.getByTestId("quickstart-start-another").click();

  // 2. Drop the CSV; the workspace may need to start first (the page holds the file meanwhile).
  const attempts = await dropFile();
  check("drop", attempts > 0, { attempts, file: csv.split("/").pop(), errors: await errorText(page) });
  if (!attempts) throw new Error("the drop zone never took the file");
  await page.getByTestId("quickstart-start").click();
  const searchable = await until(page, () => visible("quickstart-search"), { minutes: 10, every: 3000 });
  await shot("search-ready");
  check("dataset-and-index", searchable, { dataset: await stepState("dataset"), index: await stepState("search_index"), errors: await errorText(page) });
  if (!searchable) throw new Error("the search index was not ready in ten minutes");

  // 3. Search on the page.
  await page.getByTestId("quickstart-search").fill("space");
  await page.getByTestId("quickstart-hit").first().waitFor({ timeout: 30000 });
  check("search", (await page.getByTestId("quickstart-hit").count()) > 0, { hits: await page.getByTestId("quickstart-hit").count() });
  await shot("searched");

  // 4. A browser key for the website's origin (lake#1152) and the plain web page code, run there.
  await page.getByTestId("quickstart-site").fill(new URL(siteUrl).origin);
  await page.getByTestId("quickstart-create-key").click();
  await page.getByText("Your website key").waitFor({ timeout: 30000 });
  sitePage = `<!doctype html><meta charset="utf-8"><title>site</title>\n${await websiteCode()}`;
  await shot("website-code");
  const website = await context.newPage();
  await website.goto(siteUrl);
  await website.fill("#lake-query", "thieves");
  await website.waitForSelector("#lake-hits li", { timeout: 30000 });
  check("website-search", (await website.locator("#lake-hits li").count()) > 0, { results: await website.locator("#lake-hits li").count() });
  await shotOf(website, "website-search");

  // 5. Step 4 is the Ask Orama conversation about the dataset (lake#1182): it opens by itself.
  await page.getByTestId("quickstart-conversation").waitFor({ timeout: 120000 });
  const conversation = await page.getByTestId("quickstart-conversation").getAttribute("data-conversation");
  const first = await until(page, answered, { minutes: 6, every: 3000 });
  await shot("conversation-opened");
  check("conversation-opened", first, { conversation, errors: await errorText(page) });
  let proposed = await modelProposal();
  if (!proposed) {
    // No model proposed yet: ask for one, as a person would.
    const box = page.getByTestId("quickstart-conversation").locator("textarea[name=message]");
    await box.fill("Let's train a model that predicts whether a film is popular. Propose it so I can confirm.");
    await box.press("Enter");
    await page.waitForTimeout(3000);
    await until(page, async () => (await answered()) && !!(await modelProposal()), { minutes: 6, every: 3000 });
    proposed = await modelProposal();
  }
  await shot("conversation-proposal");
  check("model-proposed", !!proposed, { conversation, errors: await errorText(page) });
  if (!proposed) throw new Error("Ask Orama did not propose train_quickstart");
  await proposed.confirm.click();
  await until(page, async () => (await stepState("model")) !== "pending", { minutes: 2, every: 2000 });
  note("model-confirmed", { model: await stepState("model") });
  // lake#1204: the confirm moves step 4 to its training at once, and the conversation says what
  // started and what comes next, never "Done: train_quickstart".
  const moved = await until(page, () => visible("quickstart-train-card"), { minutes: 1, every: 1000 });
  const outcome = await page.getByTestId("quickstart-conversation").locator(".ask-agent").last().innerText().catch(() => "");
  check("confirm-moves-to-training", moved && !/Done: train_quickstart/.test(outcome), { view: await page.getByTestId("quickstart-train").getAttribute("data-view").catch(() => null), outcome: outcome.slice(0, 300) });
  if (await visible("quickstart-training")) {
    check("training-says-what-next", /This page follows it/.test(await page.getByTestId("quickstart-training").innerText()), {});
    await shot("training");
  }

  // 6. Training is followed and the page publishes the model.
  const published = await until(page, async () => (await stepState("model_answers")) === "done" || (await stepState("model")) === "failed", { minutes: 15, every: 3000 });
  await shot("trained");
  if ((await stepState("model")) === "failed") await failureShown("model-failed");
  check("model", published && (await stepState("model_answers")) === "done", { model: await stepState("model"), answers: await stepState("model_answers"), errors: await errorText(page) });
  if ((await stepState("model_answers")) !== "done") throw new Error("the model was not published");

  // 7. The model's answers on the page and, with a new key, on the website.
  await page.getByTestId("quickstart-search").fill("dreams");
  await page.getByTestId("quickstart-answer").first().waitFor({ timeout: 30000 });
  check("page-answers", (await page.getByTestId("quickstart-answer").count()) > 0, { answers: await page.getByTestId("quickstart-answer").count() });
  await page.getByTestId("quickstart-site").fill(new URL(siteUrl).origin);
  await page.getByTestId("quickstart-create-key").click();
  await page.getByText("Your website key").waitFor({ timeout: 30000 });
  sitePage = `<!doctype html><meta charset="utf-8"><title>site</title>\n${await websiteCode()}`;
  await website.goto(siteUrl);
  await website.fill("#lake-query", "dreams");
  await website.waitForSelector("#lake-hits li strong", { timeout: 30000 });
  check("website-answers", (await website.locator("#lake-hits li strong").count()) > 0, { results: await website.locator("#lake-hits li").count(), answers: await website.locator("#lake-hits li strong").count() });
  await shotOf(website, "website-answers");
  await shot("done");

  // 8. A failure shows the reason and a next action (lake#1204): a one-row dataset cannot be
  // clustered, so confirming Ask Orama's clustering proposal fails on the model step.
  await page.goto(`${base}/`, { waitUntil: "domcontentloaded" });
  if (!(await settled())) throw new Error("the Quickstart page did not read its status");
  if (await visible("quickstart-start-another")) await page.getByTestId("quickstart-start-another").click();
  fs.writeFileSync(csv, "title,rating\nOnly one film,8.1\n");
  if (!(await dropFile())) throw new Error("the drop zone never took the one-row file");
  await page.getByTestId("quickstart-start").click();
  if (!(await until(page, () => visible("quickstart-search"), { minutes: 10, every: 3000 }))) throw new Error("the one-row search index was not ready");
  await page.getByTestId("quickstart-conversation").waitFor({ timeout: 120000 });
  await until(page, answered, { minutes: 6, every: 3000 });
  const box = page.getByTestId("quickstart-conversation").locator("textarea[name=message]");
  await box.fill("Group the rows with clustering (methodology M-3). Propose it so I can confirm.");
  await box.press("Enter");
  await page.waitForTimeout(3000);
  await until(page, async () => (await answered()) && !!(await modelProposal()), { minutes: 6, every: 3000 });
  const clustering = await modelProposal();
  check("clustering-proposed", !!clustering, { errors: await errorText(page) });
  if (clustering) await clustering.confirm.click();
  const failedShown = !!clustering && (await until(page, () => visible("quickstart-failed"), { minutes: 3, every: 2000 }));
  check("failure-path", failedShown, { model: await stepState("model").catch(() => null) });
  if (failedShown) await failureShown("failure-path-shown");

  // 9. Ask Orama has its own page.
  await page.goto(`${base}/ask`, { waitUntil: "domcontentloaded" });
  await page.waitForTimeout(2000);
  await shot("ask-orama");
  note("ask-orama", { url: page.url() });
} catch (error) {
  check("error", false, { message: String(error?.message ?? error), errors: await errorText(page).catch(() => "") });
  await shot("error");
} finally {
  await browser.close();
  site.close();
}
process.exit(failed() ? 1 : 0);
