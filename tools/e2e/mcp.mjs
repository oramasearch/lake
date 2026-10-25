// Developer → MCP in a browser (docs/api/mcp.md): sign in, open the page from the rail, create a
// read-only MCP key, copy the Claude Code command through its Copy button, check every client's
// setup block carries the key, run initialize, tools/list and a refused write with the key, fetch
// the lake skill, prove the skill and key from a fresh Claude Code session (`claude -p`, one read
// call), then revoke the key under Your Keys and check it is refused. The key is never printed or
// screenshotted.
//
//   node tools/e2e/mcp.mjs <base-url> <ticket-file> <output-dir>
//
// Needs the `claude` CLI on PATH for the fresh-session check (skipped, and reported, without it).
// lib.mjs finds Playwright. Not C++: keep it out of git clang-format.
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { args, loadChromium, logger, newPage, signIn } from "./lib.mjs";

const { base, ticketFile, out } = args("node tools/e2e/mcp.mjs <base-url> <ticket-file> <output-dir>");
const chromium = await loadChromium();
const { note, check, failed } = logger(out);
const { browser, page } = await newPage(chromium, base);
const keyName = `e2e-mcp-${Date.now().toString(36)}`;
let key = "";
const scratch = fs.mkdtempSync(path.join(os.tmpdir(), "lake-mcp-"));
const rpc = async (body) => {
  const res = await fetch(`${base}/mcp`, { method: "POST", headers: { "Content-Type": "application/json", Authorization: `Bearer ${key}` }, body: JSON.stringify(body) });
  const text = await res.text();
  let json = null;
  try { json = JSON.parse(text); } catch {}
  return { status: res.status, body: json ?? {}, text };
};
try {
  const viewer = await signIn(page, base, ticketFile);
  note("sign-in", { url: page.url() });

  // The rail's Developer group opens on API and lists MCP under it.
  const rail = page.getByRole("navigation", { name: "Navigation" });
  await rail.getByRole("link", { name: "Developer", exact: true }).click();
  await rail.getByRole("link", { name: "MCP", exact: true }).click();
  await page.waitForURL(`${base}/developer/mcp`);
  await page.getByRole("heading", { level: 1, name: "MCP" }).waitFor();
  await page.screenshot({ path: `${out}/01-mcp.png`, fullPage: true });
  const trail = await page.getByRole("navigation", { name: "Breadcrumb" }).innerText();
  check("page", /Developer\s*›\s*MCP/.test(trail), { breadcrumb: trail.replace(/\s+/g, " ") });

  // A read-only key, named so its row can be found again.
  await page.getByRole("textbox", { name: "Name", exact: true }).fill(keyName);
  await page.getByRole("radio", { name: /^Read-only tools/ }).check();
  await page.getByRole("button", { name: "Create an MCP key" }).click();
  await page.locator("#mcp-key").waitFor({ timeout: 30000 });
  key = (await page.locator("#mcp-key").innerText()).trim();
  check("key", key.length > 20, { length: key.length });

  // Claude Code is the client shown first; its Copy button copies exactly its block.
  const block = await page.locator("#mcp-claude-code").innerText();
  await page.locator("button[data-copy='mcp-claude-code']").click();
  const copied = await page.evaluate(() => navigator.clipboard.readText());
  check("copy", copied === block && copied.startsWith("claude mcp add --transport http lake") && copied.includes(key),
        { button: await page.locator("button[data-copy='mcp-claude-code']").innerText() });

  // Every other client's setup: each block that names a credential carries the new key, not the
  // $LAKE_KEY placeholder. (VS Code asks for the key itself; the web connectors take none.)
  const clients = await page.locator("[data-client]").evaluateAll((all) => all.map((b) => b.getAttribute("data-client")));
  const snippets = {};
  for (const client of clients) {
    await page.locator(`[data-client='${client}']`).click();
    const codes = await page.locator("code[id^='mcp-']").evaluateAll((all) => all.filter((c) => c.id !== "mcp-key").map((c) => c.textContent ?? ""));
    snippets[client] = { blocks: codes.length, key: codes.some((c) => c.includes(key)), placeholder: codes.some((c) => c.includes("$LAKE_KEY")) };
  }
  const missing = Object.entries(snippets).filter(([client, s]) => !["vscode", "web"].includes(client) && (!s.key || s.blocks === 0)).map(([client]) => client);
  check("client snippets", clients.length >= 8 && missing.length === 0,
        { clients: Object.fromEntries(Object.entries(snippets).map(([c, s]) => [c, `${s.blocks} blocks${s.key ? ", key" : ""}`])), missing });
  await page.locator("[data-client='claude-code']").click();

  const init = await rpc({ jsonrpc: "2.0", id: 1, method: "initialize", params: { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "e2e", version: "1" } } });
  check("initialize", init.status === 200 && !!init.body.result, { server: init.body.result?.serverInfo });
  const tools = await rpc({ jsonrpc: "2.0", id: 2, method: "tools/list" });
  check("tools/list", (tools.body.result?.tools ?? []).length > 100, { tools: tools.body.result?.tools?.length });
  const read = await rpc({ jsonrpc: "2.0", id: 3, method: "tools/call", params: { name: "server_info", arguments: {} } });
  check("read call", read.status === 200 && read.body.result && !read.body.result.isError, { status: read.status });
  // A read-only key is refused every write; nothing is created.
  const write = await rpc({ jsonrpc: "2.0", id: 4, method: "tools/call", params: { name: "create_repository", arguments: { customer_id: viewer.customer_id, workspace_id: viewer.workspace_id, name: `${keyName}-refused` } } });
  const refusal = write.body.result?.content?.[0]?.text ?? write.body.error?.message ?? "";
  check("write refused", (write.body.result?.isError === true || !!write.body.error) && /does not grant|forbidden|read-only|permission/i.test(refusal),
        { status: write.status, refusal: refusal.slice(0, 200) });

  // The skill as an agent finds it: linked from the public llms.txt, fetched with no credential.
  const llms = await (await fetch(`${base}/llms.txt`)).text();
  const skillUrl = (llms.match(/\((https?:\/\/[^)\s]+\/SKILL\.md)\)/) || [])[1];
  check("skill-link", !!skillUrl, { url: skillUrl });
  const skill = await fetch(skillUrl ?? `${base}/skills/lake/SKILL.md`);
  const skillText = await skill.text();
  fs.writeFileSync(`${out}/SKILL.md`, skillText);
  check("skill", skill.status === 200 && skillText.startsWith("---\nname: lake"), { disposition: skill.headers.get("content-disposition"), bytes: skillText.length });

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
  check("error", false, { message: String(error).slice(0, 300), url: page.url() });
  await page.screenshot({ path: `${out}/failure.png`, fullPage: true }).catch(() => {});
} finally {
  fs.rmSync(scratch, { recursive: true, force: true });
  if (key) {
    // Revoke the key from Your Keys on the same page.
    try {
      await page.goto(`${base}/developer/mcp#keys`);
      const row = page.getByRole("row").filter({ hasText: keyName });
      await row.getByRole("button", { name: "Revoke" }).click();
      await page.getByRole("dialog").getByRole("button", { name: "Revoke" }).click();
      await row.waitFor({ state: "detached", timeout: 15000 }).catch(() => {});
    } catch (error) {
      note("revoke-ui", { error: String(error).slice(0, 200) });
    }
    const after = await rpc({ jsonrpc: "2.0", id: 5, method: "tools/call", params: { name: "server_info", arguments: {} } });
    check("revoked", after.status !== 200 || after.text.includes("\"isError\":true"), { status: after.status });
  }
  await browser.close();
  process.exit(failed() ? 1 : 0);
}
