// Browser walkthrough of the quickstart process (lake#1128, docs/ui/quickstart.md): Clerk sign-in
// with a one-time ticket, which lands on Quickstart at `/`; then a CSV dropped on the page, the
// live search, a website key and the plain web page code, which it serves from another origin and
// searches there; then training, publishing and the model's answers on the page and on the website.
// A screenshot per step and a log.json. It leaves the dataset, index, model and key in the
// signed-in account's workspace; delete them from Data, Ship and Tokens after a run.
//
//   node tools/e2e/quickstart.mjs <base-url> <ticket-file> <output-dir>
//
// Playwright comes from the sibling orama-website-v2 checkout, as walkthrough.mjs; set PLAYWRIGHT to
// point at another playwright package. Not C++: keep it out of git clang-format.
import fs from "node:fs";
import http from "node:http";

const candidates = [
  process.env.PLAYWRIGHT,
  "playwright",
  new URL("../../../../../orama-website-v2/node_modules/playwright/index.mjs", import.meta.url).href,
].filter(Boolean);
let chromium;
for (const spec of candidates) { try { ({ chromium } = await import(spec)); break; } catch {} }
if (!chromium) { console.error(`Playwright not found. Tried:\n  ${candidates.join("\n  ")}`); process.exit(1); }

const [base, ticketFile, out] = process.argv.slice(2);
if (!base || !ticketFile || !out) { console.error("usage: quickstart.mjs <base-url> <ticket-file> <output-dir>"); process.exit(2); }
const ticket = fs.readFileSync(ticketFile, "utf8").trim();
fs.mkdirSync(out, { recursive: true });
const log = [];
const note = (step, detail) => { log.push({ step, ...detail }); console.log(step, JSON.stringify(detail)); };

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
const site = http.createServer((_, res) => { res.writeHead(200, { "Content-Type": "text/html; charset=utf-8" }); res.end(sitePage); });
await new Promise((resolve) => site.listen(0, "127.0.0.1", resolve));
const siteUrl = `http://127.0.0.1:${site.address().port}/`;

const browser = await chromium.launch();
const context = await browser.newContext({ viewport: { width: 1440, height: 1000 } });
const page = await context.newPage();
let n = 0;
const shot = async (name, on = page) => on.screenshot({ path: `${out}/${String(++n).padStart(2, "0")}-${name}.png`, fullPage: true });
const stepState = (step) => page.getByTestId(`quickstart-step-${step}`).getAttribute("data-state");
const until = async (check, ms) => { const end = Date.now() + ms; while (Date.now() < end) { if (await check()) return true; await page.waitForTimeout(3000); } return false; };
let failed = false;

try {
  // 1. Sign in: an account lands on Quickstart at `/`.
  await page.goto(`${base}/sign-in?__clerk_ticket=${encodeURIComponent(ticket)}`);
  await page.waitForURL((url) => new URL(url).pathname === "/", { timeout: 60000 });
  await page.getByTestId("quickstart-page").waitFor({ timeout: 30000 });
  await shot("quickstart-home");
  note("sign-in", { url: page.url(), quickstart: true });
  if (await page.getByTestId("quickstart-start-another").count()) await page.getByTestId("quickstart-start-another").click();

  // 2. Drop the CSV; the workspace may need to start first (the page holds the file meanwhile).
  await page.getByTestId("quickstart-drop").locator("input[type=file]").setInputFiles(csv);
  if (await page.getByTestId("quickstart-top-up").count()) {
    note("balance", { blocked: true, link: await page.getByTestId("quickstart-top-up").getAttribute("href") });
    throw new Error("the account's balance is blocked: add funds and run again");
  }
  await page.getByTestId("quickstart-start").click();
  const searchable = await until(async () => (await page.getByTestId("quickstart-search").count()) > 0, 10 * 60 * 1000);
  await shot("search-ready");
  note("dataset-and-index", { searchable, dataset: await stepState("dataset"), index: await stepState("search_index") });
  if (!searchable) throw new Error("the search index was not ready in ten minutes");

  // 3. Search on the page.
  await page.getByTestId("quickstart-search").fill("space");
  await page.getByTestId("quickstart-hit").first().waitFor({ timeout: 30000 });
  note("search", { hits: await page.getByTestId("quickstart-hit").count() });
  await shot("searched");

  // 4. A website key and the plain web page code, run from another origin.
  await page.getByTestId("quickstart-create-key").click();
  await page.getByText("Your website key").waitFor({ timeout: 30000 });
  sitePage = `<!doctype html><meta charset="utf-8"><title>site</title>\n${await page.getByTestId("quickstart-snippet-html").locator("code").innerText()}`;
  await shot("website-code");
  const website = await context.newPage();
  await website.goto(siteUrl);
  await website.fill("#lake-query", "thieves");
  await website.waitForSelector("#lake-hits li", { timeout: 30000 });
  note("website-search", { results: await website.locator("#lake-hits li").count() });
  await shot("website-search", website);

  // 5. Train, follow the progress, and let the page publish it.
  await page.getByTestId("quickstart-train-button").click();
  const published = await until(async () => (await stepState("model_answers")) === "done" || (await stepState("model")) === "failed", 15 * 60 * 1000);
  await shot("trained");
  note("model", { published, model: await stepState("model"), answers: await stepState("model_answers") });
  if ((await stepState("model_answers")) !== "done") throw new Error("the model was not published");

  // 6. The model's answers on the page and, with a new key, on the website.
  await page.getByTestId("quickstart-search").fill("dreams");
  await page.getByTestId("quickstart-answer").first().waitFor({ timeout: 30000 });
  note("page-answers", { answers: await page.getByTestId("quickstart-answer").count() });
  await page.getByTestId("quickstart-create-key").click();
  await page.getByText("Your website key").waitFor({ timeout: 30000 });
  sitePage = `<!doctype html><meta charset="utf-8"><title>site</title>\n${await page.getByTestId("quickstart-snippet-html").locator("code").innerText()}`;
  await website.goto(siteUrl);
  await website.fill("#lake-query", "dreams");
  await website.waitForSelector("#lake-hits li strong", { timeout: 30000 });
  note("website-answers", { results: await website.locator("#lake-hits li").count(), answers: await website.locator("#lake-hits li strong").count() });
  await shot("website-answers", website);
  await shot("done");

  // 7. Ask Orama has its own page.
  await page.goto(`${base}/ask`);
  await page.waitForLoadState("load");
  await shot("ask-orama");
  note("ask-orama", { url: page.url() });
} catch (error) {
  failed = true;
  note("error", { message: String(error?.message ?? error) });
  await shot("error").catch(() => {});
} finally {
  fs.writeFileSync(`${out}/log.json`, JSON.stringify(log, null, 2));
  await browser.close();
  site.close();
}
process.exit(failed ? 1 : 0);
