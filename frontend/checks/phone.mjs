// Phone checks for lake's web app at 390×844, an iPhone user agent, touch and DPR 3
// (docs/ui/mobile-web.md → How to Check It). Run against `npx vite --port 5174`; every /v1 call is
// answered by the mocks below, so no lake is needed. Playwright is not a dependency of the app:
//
//   PLAYWRIGHT_FROM=/path/to/a/project/with/playwright/package.json OUT=/tmp/shots node checks/phone.mjs
//
// ENGINE=webkit (with WK=<webkit pw_run.sh> for a non-default build), VISITOR=1 for the signed-out
// shell, STT=off for a lake without speech-to-text, PAGES=/a,/b for a sweep of other paths.
// It prints a JSON report: each page's width, small targets and fields under 16 px or 40 px, then
// the top bar, the drawer, the New Transcript sheet with the keyboard simulated as iOS does it
// (visualViewport 508 px), and Ask Orama opened about a transcript.
import { createRequire } from "node:module";
import { mkdirSync } from "node:fs";
const require = createRequire(process.env.PLAYWRIGHT_FROM ?? import.meta.url);
const { chromium, webkit, devices } = require("playwright");

const OUT = process.env.OUT ?? "/tmp/lake-phone-shots";
mkdirSync(OUT, { recursive: true });
const BASE = process.env.BASE ?? "http://localhost:5174";
const engine = process.env.ENGINE === "webkit" ? webkit : chromium;
const visitor = process.env.VISITOR === "1";
const stt = process.env.STT !== "off";

const viewer = visitor
  ? { signed_in: false, administrator: false, spaces: [], clerk: { enabled: false }, password_sign_up: true, version: "dev" }
  : {
      signed_in: true,
      administrator: false,
      display_name: "Ada Lovelace",
      email: "ada@example.com",
      customer_id: "c1",
      workspace_id: "ws1",
      spaces: [{ id: "c1", name: "ada", label: "Ada", personal: true, workspaces: [{ id: "ws1", name: "Main workspace with a long name", customer_id: "c1", status: "ready" }] }],
      clerk: { enabled: false },
      password_sign_up: false,
      version: "dev",
    };

const text = Array.from({ length: 40 }, (_, i) => `Speaker ${i % 2 ? "B" : "A"}: line ${i + 1} about https://lake.example.com/w/0a1b2c3d4e5f60718293a4b5c6d7e8f9/design/project?name=a-very-long-project-name-without-spaces and the pipeline.`).join("\n");
const rows = [
  { id: "t1", title: "Weekly sync with the design team about the pipeline", excerpt: "Speaker A: we talked about the pipeline and the next release of the app, which ships on Friday.", source: "recording", seconds: 1832, created_at: "2026-10-09T17:05:00Z", created_by: "ada@example.com" },
  { id: "t2", title: "call_2026-10-08", excerpt: "A call recorded on the phone.", source: "upload", seconds: 312, created_at: "2026-10-08T10:00:00Z", created_by: "ada@example.com" },
  { id: "t3", title: "Notes pasted from a meeting", excerpt: "averyveryverylongwordwithoutanyspacesthatcouldwidenthepageifnotwrappedproperly-0123456789", source: "text", seconds: null, created_at: "2026-10-01T10:00:00Z" },
];

const answers = {
  viewer,
  workspace_status: { requested: true, ready: true, phase: "Ready", workspace_id: "ws1" },
  list_transcripts: { transcripts: rows, next_cursor: "n1" },
  get_transcript: { ...rows[0], text, language: "en", recorded_at: "2026-10-09T17:05:00Z" },
  create_transcript: { ...rows[0], id: "t9", text: "hello" },
  list_agent_library: { agents: [] },
  workspace_query: { agents: [], projects: [] },
  get_ask_conversation: {
    messages: Array.from({ length: 14 }, (_, i) => ({ id: `m${i}`, role: i % 2 ? "assistant" : "user", content: i % 2 ? "An answer with **Markdown** and a long line ".repeat(6) : `Question ${i}` })),
  },
  send_ask_message: { conversation_id: "cv1", task_id: "" },
  list_repositories: { repositories: [] },
};

const browser = await engine.launch(process.env.WK ? { executablePath: process.env.WK } : {});
const context = await browser.newContext({ ...devices["iPhone 13"], viewport: { width: 390, height: 844 } });
const calls = [];
await context.route(/\/v1\/[a-z_]+/, async (route) => {
  const url = new URL(route.request().url());
  const name = url.pathname.split("/v1/")[1];
  calls.push({ name, body: route.request().postData() });
  if (name === "transcribe" || (!stt && name === "create_transcript")) {
    if (!stt) return route.fulfill({ status: 400, contentType: "application/json", body: JSON.stringify({ ok: false, error: { code: "not_supported", message: "Speech-to-text is not configured on this lake." } }) });
    return route.fulfill({ contentType: "application/json", body: JSON.stringify({ ok: true, value: { text: "dictated words", model: "test" } }) });
  }
  const value = answers[name] ?? {};
  return route.fulfill({ contentType: "application/json", body: JSON.stringify({ ok: true, value }) });
});
const page = await context.newPage();
const errors = [];
page.on("pageerror", (e) => errors.push(String(e)));

async function overflow() {
  return page.evaluate(() => {
    const W = 390;
    const scrollers = (el) => {
      for (let p = el.parentElement; p; p = p.parentElement) {
        const s = getComputedStyle(p);
        if (["auto", "scroll", "hidden"].includes(s.overflowX) && p.scrollWidth > p.clientWidth + 1) return true;
      }
      return false;
    };
    const wide = [];
    for (const el of document.querySelectorAll("body *")) {
      const r = el.getBoundingClientRect();
      if (!r.width || r.right <= W + 1) continue;
      if (getComputedStyle(el).position === "fixed") continue;
      if (el.closest("nav[aria-label='Navigation']")) continue; // the off-canvas drawer
      if (r.left >= W) continue;
      if (scrollers(el)) continue;
      if (el.children.length && [...el.children].some((c) => c.getBoundingClientRect().right > W + 1)) continue;
      wide.push(`${el.tagName.toLowerCase()}.${String(el.className).slice(0, 60)} right=${Math.round(r.right)} "${(el.textContent || "").trim().slice(0, 40)}"`);
    }
    const tw = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
    let n;
    while ((n = tw.nextNode())) {
      const el = n.parentElement;
      if (!el || el.closest("nav[aria-label='Navigation']") || scrollers(el)) continue;
      const range = document.createRange();
      range.selectNodeContents(n);
      const b = range.getBoundingClientRect();
      if (b.width && b.right > W + 1 && b.left < W) wide.push(`text in ${el.tagName.toLowerCase()}.${String(el.className).slice(0, 40)} right=${Math.round(b.right)} "${n.textContent.trim().slice(0, 40)}"`);
    }
    return { scrollWidth: document.documentElement.scrollWidth, W, wide: wide.slice(0, 8) };
  });
}

async function targets() {
  // Visible controls in the top bar and kit buttons, and their hit areas.
  return page.evaluate(() => {
    const small = [];
    for (const el of document.querySelectorAll("header button, header a[href], main .button, main button")) {
      const r = el.getBoundingClientRect();
      if (!r.width || !r.height) continue;
      const before = getComputedStyle(el, "::before");
      const bw = parseFloat(before.width) || 0;
      const bh = parseFloat(before.height) || 0;
      const w = Math.max(r.width, bw);
      const h = Math.max(r.height, bh);
      if (w < 24 || h < 24) small.push(`${el.tagName} "${(el.getAttribute("aria-label") || el.textContent || "").trim().slice(0, 30)}" ${Math.round(w)}×${Math.round(h)}`);
    }
    return small.slice(0, 10);
  });
}

async function fields() {
  return page.evaluate(() =>
    [...document.querySelectorAll("input:not([type=hidden]):not([type=checkbox]):not([type=radio]):not([type=file]), textarea, select")]
      .filter((el) => el.getBoundingClientRect().width)
      .map((el) => ({ name: el.getAttribute("aria-label") || el.name || el.id || el.tagName, font: getComputedStyle(el).fontSize, h: Math.round(el.getBoundingClientRect().height) }))
      .filter((f) => f.font !== "16px" || f.h < 40),
  );
}

const only = process.env.PAGES ? process.env.PAGES.split(",") : null;
const pages = only ?? (visitor
  ? ["/", "/repositories", "/sign-in", "/docs"]
  : ["/", "/repositories", "/search", "/w/ws1/train/data", "/w/ws1/design/projects", "/w/ws1/train/agents", "/w/ws1/train/runs", "/w/ws1/ship/models", "/workspaces", "/settings", "/organizations", "/w/ws1/ask", "/w/ws1/transcripts", "/w/ws1/transcript?id=t1"]);

const report = {};
for (const path of pages) {
  await page.goto(BASE + path, { waitUntil: "networkidle" });
  await page.waitForTimeout(400);
  const slug = path.replace(/[^a-z0-9]+/gi, "_").replace(/^_|_$/g, "") || "home";
  await page.screenshot({ path: `${OUT}/${process.env.ENGINE ?? "chromium"}-${slug}.png` });
  report[path] = { overflow: await overflow(), small: await targets(), fields: await fields() };
}

if (!visitor && !only) {
  // The top bar on a phone: last crumb only, icons for search, the workspace and Ask Orama.
  await page.goto(BASE + "/w/ws1/transcripts", { waitUntil: "networkidle" });
  report.topbar = await page.evaluate(() => {
    const crumbs = [...document.querySelectorAll("nav[aria-label=Breadcrumb] > span")].filter((s) => getComputedStyle(s).display !== "none").map((s) => s.textContent);
    const pill = document.querySelector("button[aria-haspopup=listbox]");
    const ask = [...document.querySelectorAll("header button")].find((b) => b.getAttribute("aria-label") === "Ask Orama");
    return { crumbs, pill: pill && Math.round(pill.getBoundingClientRect().width), ask: ask && Math.round(ask.getBoundingClientRect().width), header: Math.round(document.querySelector("header").getBoundingClientRect().height) };
  });

  // The rail drawer.
  await page.click("button[aria-label='Open the navigation']");
  await page.waitForTimeout(300);
  await page.screenshot({ path: `${OUT}/${process.env.ENGINE ?? "chromium"}-drawer.png` });
  report.drawer = await page.evaluate(() => {
    const n = document.querySelector("nav[aria-label=Navigation]").getBoundingClientRect();
    return { left: n.left, width: n.width, transcripts: !!document.querySelector("nav[aria-label=Navigation] a[href$='/transcripts']") };
  });
  await page.click("button[aria-label='Close the navigation']");
  await page.waitForTimeout(300);

  // New transcript: a full-screen sheet; Paste; keyboard simulated as iOS does.
  await page.click("main button:has-text(\"New transcript\")");
  await page.waitForTimeout(300);
  await page.screenshot({ path: `${OUT}/${process.env.ENGINE ?? "chromium"}-new.png` });
  report.sheet = await page.evaluate(() => {
    const d = document.querySelector("[role=dialog]").getBoundingClientRect();
    return { top: d.top, height: Math.round(d.height), width: Math.round(d.width), locked: document.documentElement.dataset.sheet };
  });
  await page.click("[role=dialog] button:has-text(\"From your phone\")");
  await page.waitForTimeout(200);
  await page.screenshot({ path: `${OUT}/${process.env.ENGINE ?? "chromium"}-new-phone-help.png` });
  report.phoneHelp = await page.evaluate(() => [...document.querySelectorAll("section[aria-label='From your phone'] button")].map((b) => `${b.textContent || b.getAttribute("aria-label")} ${Math.round(b.getBoundingClientRect().height)}`));
  await page.click("[role=dialog] button:has-text(\"Type, paste or dictate\")");
  await page.waitForTimeout(200);
  await page.fill("#transcript-text", "Kickoff with Acme\nAda: hello there.");
  await page.focus("#transcript-text");
  await page.evaluate(() => {
    Object.defineProperty(window.visualViewport, "height", { configurable: true, get: () => 508 });
    Object.defineProperty(window.visualViewport, "offsetTop", { configurable: true, get: () => 0 });
    window.visualViewport.dispatchEvent(new Event("resize"));
  });
  await page.waitForTimeout(300);
  await page.screenshot({ path: `${OUT}/${process.env.ENGINE ?? "chromium"}-paste-keyboard.png` });
  report.paste = await page.evaluate(() => {
    const save = [...document.querySelectorAll(".sheet-actions button")].find((b) => /Save/.test(b.textContent));
    const r = save?.getBoundingClientRect();
    return { saveBottom: r && Math.round(r.bottom), saveHeight: r && Math.round(r.height), dialogHeight: Math.round(document.querySelector("[role=dialog]").getBoundingClientRect().height), fields: [...document.querySelectorAll("[role=dialog] input, [role=dialog] textarea")].map((f) => getComputedStyle(f).fontSize) };
  });
  await page.evaluate(() => {
    Object.defineProperty(window.visualViewport, "height", { configurable: true, get: () => 844 });
    window.visualViewport.dispatchEvent(new Event("resize"));
  });
  await page.click(".sheet-actions button:has-text('Save')");
  await page.waitForTimeout(500);
  report.created = { url: page.url(), call: calls.filter((c) => c.name === "create_transcript").map((c) => c.body).pop() };
  report.unlocked = await page.evaluate(() => ({ sheet: document.documentElement.dataset.sheet ?? null, bodyPosition: document.body.style.position }));

  // Ask Orama about this: full screen, context chip, composer above the keyboard.
  await page.goto(BASE + "/w/ws1/transcript?id=t1", { waitUntil: "networkidle" });
  await page.click("text=Ask Orama about this");
  await page.waitForTimeout(800);
  await page.screenshot({ path: `${OUT}/${process.env.ENGINE ?? "chromium"}-ask.png` });
  report.ask = await page.evaluate(() => {
    const p = document.querySelector(".ask-panel").getBoundingClientRect();
    const box = document.querySelector(".ask-panel textarea");
    return { top: p.top, width: Math.round(p.width), height: Math.round(p.height), draft: box.value, chip: document.querySelector(".ask-context")?.textContent, font: getComputedStyle(box).fontSize, mic: !!document.querySelector(".ask-panel button[aria-label=Dictate]") };
  });
  // Scrolled to the latest answer?
  report.askFollow = await page.evaluate(() => {
    const s = document.querySelector(".ask-scroll");
    return { atEnd: s.scrollHeight - s.scrollTop - s.clientHeight < 80 };
  });
  await page.focus(".ask-panel textarea");
  await page.evaluate(() => {
    Object.defineProperty(window.visualViewport, "height", { configurable: true, get: () => 508 });
    window.visualViewport.dispatchEvent(new Event("resize"));
  });
  await page.waitForTimeout(300);
  await page.screenshot({ path: `${OUT}/${process.env.ENGINE ?? "chromium"}-ask-keyboard.png` });
  report.askKeyboard = await page.evaluate(() => {
    const send = document.querySelector(".ask-panel .ask-send").getBoundingClientRect();
    return { sendBottom: Math.round(send.bottom), sendHeight: Math.round(send.height), panelHeight: Math.round(document.querySelector(".ask-panel").getBoundingClientRect().height) };
  });
  await page.evaluate(() => {
    Object.defineProperty(window.visualViewport, "height", { configurable: true, get: () => 844 });
    window.visualViewport.dispatchEvent(new Event("resize"));
  });
  await page.click(".ask-panel .ask-send");
  await page.waitForTimeout(500);
  report.askSent = calls.filter((c) => c.name === "send_ask_message").map((c) => JSON.parse(c.body)).map((b) => ({ message: b.message, page: { ...b.page, transcript: `${(b.page?.transcript ?? "").length} chars` } }));
}

report.errors = errors;
console.log(JSON.stringify(report, null, 1));
await browser.close();
