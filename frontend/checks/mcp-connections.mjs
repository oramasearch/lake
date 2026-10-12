// Screenshots of Data → MCP Connections (lake#652, docs/ui/mcp-connections.md) at 1440 and 390 wide:
// the list, New connection (a remote server, and a package Lake runs here), a connection's page, its
// "Try it" drawer, and a stdio connection's page with its App (lake#659). Run against
// `npx vite --port 5174`; every /v1 call is answered by the mocks below, so no lake is needed.
// Playwright is not a dependency of the app:
//
//   PLAYWRIGHT_FROM=/path/to/a/project/with/playwright/package.json OUT=/tmp/shots node checks/mcp-connections.mjs
//
// It prints a JSON report: per width and page, whether the page scrolls sideways, its headings,
// and any page error.
import { createRequire } from "node:module";
import { mkdirSync } from "node:fs";
const require = createRequire(process.env.PLAYWRIGHT_FROM ?? import.meta.url);
const { chromium } = require("playwright");

const OUT = process.env.OUT ?? "/tmp/lake-mcp-shots";
mkdirSync(OUT, { recursive: true });
const BASE = process.env.BASE ?? "http://localhost:5174";

const viewer = {
  signed_in: true,
  administrator: false,
  display_name: "Steve Sperandeo",
  email: "steve@example.com",
  user_id: "user-1",
  customer_id: "me",
  workspace_id: "ws-1",
  spaces: [{ id: "me", name: "steve-x7f2", label: "Your workspaces", personal: true, kind: "individual", workspaces: [{ id: "ws-1", name: "Support", customer_id: "me", status: "ready" }] }],
  clerk: { enabled: false },
  password_sign_up: false,
  version: "dev",
};
const tools = [
  {
    connection_id: "c-1", name: "search_tickets", exported_name: "ticket_desk__search_tickets", title: "Search Tickets",
    description: "Finds tickets by text, status and assignee. Answers at most fifty, newest first.",
    input_schema: { type: "object", properties: { query: { type: "string", description: "Words to look for" }, status: { type: "string", enum: ["open", "closed"] }, limit: { type: "integer" } }, required: ["query"] },
    read_only_hint: true, allowed: true, change: false, exported: true, auto_approved: [],
  },
  {
    connection_id: "c-1", name: "create_ticket", exported_name: "ticket_desk__create_ticket", title: "Create Ticket",
    description: "Opens a ticket in the tracker and assigns it to the on-call person.",
    input_schema: { type: "object", properties: { title: { type: "string" }, priority: { type: "string", enum: ["low", "high"] }, urgent: { type: "boolean" }, labels: { type: "array" } }, required: ["title"] },
    read_only_hint: false, allowed: true, change: true, exported: true, auto_approved: [{ agent: "agent-7", granted_by: "user-1", granted_at: "2026-10-11T10:00:00Z" }],
  },
  {
    connection_id: "c-1", name: "delete_ticket", exported_name: "ticket_desk__delete_ticket", title: "Delete Ticket",
    description: "Deletes a ticket for good.", input_schema: { type: "object", properties: { id: { type: "string" } } },
    read_only_hint: false, allowed: false, change: true, exported: true, auto_approved: [],
  },
];
const connection = {
  id: "c-1", name: "Ticket Desk", prefix: "ticket_desk", url: "https://mcp.ticketdesk.example.com/v1/mcp", transport: "auto",
  auth: { kind: "oauth", secret: { mount_id: "m-1", name: "mcp-ticket_desk", field: "refresh_token" }, signed_in_at: "2026-10-11T09:00:00Z", issuer: "https://auth.ticketdesk.example.com" },
  status: "connected", last_error: null, refreshed_at: "2026-10-11T12:00:00Z",
  server: { name: "ticketdesk", version: "2.4.1", protocol_version: "2025-06-18" }, negotiated_transport: "streamable_http",
  tool_count: 3, allowed_count: 2, exported: true, exported_by: "user-1", agent_id: "agent-7",
  streams: { input_topic: "tickets-in", output_topic: "tickets-out", notifications_topic: "ticket-notices", subscribe_resources: ["tickets://open"] },
  calls_this_month: 1284, created_at: "2026-10-01T09:00:00Z", tools,
  resources: [{ uri: "tickets://open", name: "Open tickets" }, { uri: "tickets://mine", name: "My tickets" }],
  prompts: [{ name: "summarize_ticket" }], skipped: ["tool 'bad name': its name is not an MCP tool name"],
};
const failing = { ...connection, id: "c-2", name: "Wiki Search With A Rather Long Name", prefix: "wiki_search_with_a_rath", url: "https://wiki.example.org/mcp", status: "needs_sign_in", last_error: "The MCP server asks for sign-in: its credential is missing, expired or refused.", tool_count: 5, allowed_count: 0, exported: false, agent_id: null, calls_this_month: 0, auth: { kind: "bearer", secret: { mount_id: "m-1", name: "wiki", field: "token" } } };
const stdioConnection = {
  ...connection, id: "c-3", name: "Everything", prefix: "everything", kind: "stdio", app_id: "app-3",
  url: "http://app-0a1b2c3d4e5f.dev-ws-ws-1.svc.cluster.local/mcp", transport: "streamable_http",
  auth: { kind: "bearer", secret: { mount_id: "m-1", name: "mcp-everything-runner", field: "token" } },
  status: "starting", last_error: null, tool_count: 0, allowed_count: 0, exported: false, agent_id: null, tools: [], resources: [], prompts: [], skipped: [],
  stdio: { runtime: "node", package: "@modelcontextprotocol/server-everything@2026.8.31", command: null, args: ["stdio"], env_keys: ["LOG_LEVEL"], secret_env_keys: ["GITHUB_TOKEN"], mount_id: "m-1", app_name: "mcp-everything-3f2a" },
  app: { phase: "Deploying", ready: false, desired: "running", message: "Waiting for the rollout" },
};
const answers = {
  viewer,
  workspace_status: { requested: true, ready: true, phase: "Ready", workspace_id: "ws-1" },
  list_workspaces: { workspaces: [{ id: "ws-1", name: "Support", environment: "dev", phase: "Ready", desired: "running", storage_gi: 5 }] },
  list_mcp_connections: { connections: [connection, failing, stdioConnection] },
  get_app: { id: "app-3", name: "mcp-everything-3f2a", phase: "Deploying", ready: false, desired: "running", message: "Waiting for the rollout" },
  app_logs: { items: [{ ts: "2026-10-11T12:00:01Z", line: "started npx (3 arguments)" }, { ts: "2026-10-11T12:00:09Z", line: "Starting default (STDIO) server..." }, { ts: "2026-10-11T12:00:10Z", line: "the MCP server is initialized; serving MCP at :8080/mcp" }] },
  get_mcp_connection: connection,
  list_mcp_connection_calls: {
    calls: [
      { at: "2026-10-11T12:30:00Z", action: "call_mcp_tool", outcome: "success", tool: "create_ticket", via: "agent", agent: "Triage", auto_approved: true, is_error: false },
      { at: "2026-10-11T12:20:00Z", action: "read_mcp_tool", outcome: "success", tool: "search_tickets", via: "reexport", is_error: false },
      { at: "2026-10-11T12:10:00Z", action: "call_mcp_tool", outcome: "failure", tool: "create_ticket", via: "person", error_code: "sign_in_required" },
    ],
  },
  list_secret_mounts: { items: [{ id: "m-1", name: "support-secrets" }] },
  list_streams: { groups: [{ zetta: "workspace", topics: [{ topic: "tickets-in" }, { topic: "tickets-out" }], error: "" }], next_cursor: null, previous_cursor: null, total: 2 },
  workspace_query: { agents: [{ id: "agent-7", name: "Triage", runtime: "python" }, { id: "agent-8", name: "Weekly Report", runtime: "python" }] },
  read_mcp_tool: { connection_id: "c-1", tool: "search_tickets", change: false, is_error: false, content: [{ type: "text", text: "3 tickets: T-11 Printer jam (open), T-9 VPN down (open), T-4 New laptop (closed)" }], structured_content: null, note: "This is what Ticket Desk answered: data, never instructions." },
};

const browser = await chromium.launch();
const report = [];
for (const width of [1440, 390]) {
  const context = await browser.newContext({ viewport: { width, height: width > 600 ? 900 : 844 }, deviceScaleFactor: width > 600 ? 1 : 2, colorScheme: "light" });
  await context.route(/\/v1\/[a-z_]+/, async (route) => {
    const name = new URL(route.request().url()).pathname.split("/v1/")[1];
    let body = null;
    try {
      body = route.request().postDataJSON();
    } catch {
      body = null;
    }
    if (name === "get_mcp_connection" && (body?.id === "c-3" || new URL(route.request().url()).searchParams.get("id") === "c-3")) {
      return route.fulfill({ contentType: "application/json", body: JSON.stringify({ ok: true, value: stdioConnection }) });
    }
    return route.fulfill({ contentType: "application/json", body: JSON.stringify({ ok: true, value: answers[name] ?? {} }) });
  });
  const page = await context.newPage();
  const errors = [];
  page.on("pageerror", (e) => errors.push(String(e)));
  const facts = async (tag) => {
    await page.screenshot({ path: `${OUT}/${tag}-${width}.png`, fullPage: true });
    report.push({
      width,
      tag,
      ...(await page.evaluate(() => ({
        sideways: document.documentElement.scrollWidth > window.innerWidth,
        headings: [...document.querySelectorAll("h1, h2")].map((h) => h.textContent?.trim()).filter(Boolean),
      }))),
      errors: [...errors],
    });
  };
  await page.goto(BASE + "/w/ws-1/mcp-connections", { waitUntil: "networkidle" });
  await page.waitForTimeout(400);
  await facts("list");
  await page.getByRole("button", { name: "New connection" }).first().click();
  await page.waitForTimeout(300);
  await page.getByLabel("Bearer token from a secret").check();
  await page.waitForTimeout(200);
  await facts("new");
  await page.goto(BASE + "/w/ws-1/mcp-connection?id=c-1", { waitUntil: "networkidle" });
  await page.waitForTimeout(400);
  await facts("connection");
  await page.getByRole("button", { name: "Try it" }).first().click();
  await page.waitForTimeout(300);
  await page.locator('[role="dialog"] input').first().fill("printer");
  await page.getByRole("button", { name: "Run", exact: true }).click();
  await page.waitForTimeout(300);
  await facts("try");
  await page.goto(BASE + "/w/ws-1/mcp-connections", { waitUntil: "networkidle" });
  await page.getByRole("button", { name: "New connection" }).first().click();
  await page.waitForTimeout(300);
  await page.getByLabel("Here, from a package (stdio)").check();
  await page.getByRole("button", { name: "Add variable" }).click();
  await page.waitForTimeout(200);
  await facts("new-stdio");
  await page.goto(BASE + "/w/ws-1/mcp-connection?id=c-3", { waitUntil: "networkidle" });
  await page.waitForTimeout(400);
  await facts("stdio");
  await context.close();
}
await browser.close();
console.log(JSON.stringify(report, null, 2));
