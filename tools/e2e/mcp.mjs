// Developer → MCP in a browser (docs/api/mcp.md): sign in, open the page, create a read-only MCP
// key, copy the Claude Code command through the page's Copy button, run initialize and tools/list
// with the key, download the lake skill, prove the skill and key from a fresh Claude Code session
// (`claude -p`, one read call), then revoke the key. The key is never printed or screenshotted.
//
//   node tools/e2e/mcp.mjs <base-url> <ticket-file> <output-dir>
//
// Needs the `claude` CLI on PATH for the fresh-session check (skipped, and reported, without it).
// Playwright is found as walkthrough.mjs finds it. Not C++: keep it out of git clang-format.
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
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
fs.mkdirSync(out, { recursive: true });
const log = [];
let failed = false;
const note = (step, detail) => {
  log.push({ step, time: new Date().toISOString(), ...detail });
  console.log(new Date().toISOString().slice(11, 19), step, JSON.stringify(detail));
  fs.writeFileSync(`${out}/log.json`, JSON.stringify(log, null, 2));
};
const check = (step, passed, detail = {}) => { note(step, { passed, ...detail }); if (!passed) failed = true; };

const browser = await chromium.launch();
const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
await context.grantPermissions(["clipboard-read", "clipboard-write"], { origin: base });
const page = await context.newPage();
let key = "";
const scratch = fs.mkdtempSync(path.join(os.tmpdir(), "lake-mcp-"));
try {
  await page.goto(`${base}/sign-in?__clerk_ticket=${encodeURIComponent(fs.readFileSync(ticketFile, "utf8").trim())}`);
  await page.waitForURL(`${base}/repositories**`, { timeout: 60000 });

  await page.getByRole("link", { name: "MCP", exact: true }).click();
  await page.waitForURL(`${base}/developer/mcp`);
  await page.screenshot({ path: `${out}/01-mcp.png`, fullPage: true });
  const trail = await page.locator("nav.topbar-trail").innerText();
  check("page", (await page.locator("h1").innerText()) === "MCP", { breadcrumb: trail.replace(/\s+/g, " ") });

  await page.locator("input[name=access][value=read]").check();
  await page.getByRole("button", { name: "Create an MCP key" }).click();
  await page.waitForLoadState("load");
  key = (await page.locator("#mcp-key").innerText()).trim();
  check("key", key.length > 20, { length: key.length });

  const block = await page.locator("#mcp-claude-code").innerText();
  await page.locator("button[data-copy='mcp-claude-code']").click();
  const copied = await page.evaluate(() => navigator.clipboard.readText());
  check("copy", copied === block && copied.startsWith("claude mcp add --transport http lake") && copied.includes(key),
        { button: await page.locator("button[data-copy='mcp-claude-code']").innerText() });

  const rpc = async (body) => {
    const res = await fetch(`${base}/mcp`, { method: "POST", headers: { "Content-Type": "application/json", Authorization: `Bearer ${key}` }, body: JSON.stringify(body) });
    return { status: res.status, body: await res.json() };
  };
  const init = await rpc({ jsonrpc: "2.0", id: 1, method: "initialize", params: { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "e2e", version: "1" } } });
  check("initialize", init.status === 200 && !!init.body.result, { server: init.body.result?.serverInfo });
  const tools = await rpc({ jsonrpc: "2.0", id: 2, method: "tools/list" });
  check("tools/list", (tools.body.result?.tools ?? []).length > 100, { tools: tools.body.result?.tools?.length });

  // The skill as an agent finds it: linked from the public llms.txt, fetched with no credential.
  const llms = await (await fetch(`${base}/llms.txt`)).text();
  const skillUrl = (llms.match(/\((https?:\/\/[^)\s]+\/SKILL\.md)\)/) || [])[1];
  check("skill-link", !!skillUrl, { url: skillUrl });
  const skill = await page.request.get(skillUrl ?? `${base}/skills/lake/SKILL.md`, { headers: { cookie: "" } });
  const skillText = await skill.text();
  fs.writeFileSync(`${out}/SKILL.md`, skillText);
  check("skill", skill.status() === 200 && skillText.startsWith("---\nname: lake"),
        { disposition: skill.headers()["content-disposition"], bytes: skillText.length });

  // A fresh Claude Code session in an empty project holding only the skill and the MCP config.
  if (spawnSync("claude", ["--version"]).status === 0) {
    fs.mkdirSync(path.join(scratch, ".claude/skills/lake"), { recursive: true });
    fs.writeFileSync(path.join(scratch, ".claude/skills/lake/SKILL.md"), skillText);
    fs.writeFileSync(path.join(scratch, "mcp.json"),
      JSON.stringify({ mcpServers: { lake: { type: "http", url: `${base}/mcp`, headers: { Authorization: `Bearer ${key}` } } } }), { mode: 0o600 });
    const run = spawnSync("claude", ["-p",
      "Use the lake skill. Call the lake MCP tool server_info and answer with exactly: NAME=<name> REVISION=<revision>",
      "--mcp-config", "mcp.json", "--strict-mcp-config", "--allowedTools", "mcp__lake__server_info"],
      { cwd: scratch, encoding: "utf8", timeout: 180000 });
    const answer = (run.stdout || "").trim();
    check("claude-session", /NAME=lake\b/.test(answer) && /REVISION=\w+/.test(answer), { answer: answer.slice(0, 200), status: run.status });
  } else {
    note("claude-session", { skipped: "claude CLI not found" });
  }
} catch (error) {
  failed = true;
  note("error", { message: String(error).slice(0, 300) });
} finally {
  fs.rmSync(scratch, { recursive: true, force: true });
  if (key) {
    // Revoke the key: its row on Tokens is the newest "MCP key (read-only)".
    await page.goto(`${base}/tokens`);
    const row = page.locator("tr", { hasText: "MCP key (read-only)" }).last();
    if (await row.count()) {
      await row.getByRole("button", { name: "Revoke" }).click();
      await page.waitForLoadState("load");
    }
    const after = await fetch(`${base}/mcp`, { method: "POST", headers: { "Content-Type": "application/json", Authorization: `Bearer ${key}` },
      body: JSON.stringify({ jsonrpc: "2.0", id: 3, method: "tools/call", params: { name: "list_sessions", arguments: {} } }) });
    const text = await after.text();
    check("revoked", after.status !== 200 || text.includes("\"isError\":true"), { status: after.status });
  }
  await browser.close();
  process.exit(failed ? 1 : 0);
}
