/** Developer → MCP (`/developer/mcp`): connect an agent to lake. The endpoint, the tool groups, the
 * worked flows and the scope lists come from `mcp_setup`; the key is `create_token` with the
 * input the server's `/developer/mcp/key` handler built. The key is shown once and lives only in
 * this page's memory; every setup block carries it, or `$LAKE_KEY` before one is made. */
import { useState } from "react";
import { Link } from "react-router-dom";
import { act, useRead } from "@/lib/api";
import { Button, CopyBlock, Field, Form, Page, Query } from "@/lib/ui";
import { messageOf } from "@/pages/account/signIn";

/** A tool as `mcp_setup` lists it: its name, or the name with its description and access. */
type ToolEntry = string | { name: string; title?: string; description?: string; writes?: boolean; access?: string };
/** A flow step: the tool and what it is for, or a sentence. */
type FlowStep = string | { tool: string; note?: string };

type McpSetup = {
  origin: string;
  mcp_url: string;
  tool_groups: { title: string; summary?: string; tools: ToolEntry[] }[];
  flows: { title: string; summary?: string; steps: FlowStep[] }[];
  scopes: { read_only: string[]; all: string[] };
};

const NO_WORKSPACE = { workspace: null } as const;

type Access = "all" | "read";

/** The `create_token` input the server's MCP key handler built (serve_mcp_key). */
export function mcpKeyInput(access: Access, lifetime: string, reason: string, scopes: McpSetup["scopes"]) {
  const readOnly = access === "read";
  const input: Record<string, unknown> = {
    name: readOnly ? "MCP key (read-only)" : "MCP key",
    scopes: readOnly ? scopes.read_only : ["*"],
    static: false,
    reason: "created on Developer → MCP",
  };
  if (lifetime === "never") {
    // A key that never expires needs named scopes: "every tool" names every capability.
    input.never_expires = true;
    if (reason.trim()) input.reason = reason.trim();
    if (!readOnly) input.scopes = scopes.all;
  } else if (lifetime) {
    input.expires_in_seconds = Number.parseInt(lifetime, 10);
  }
  return input;
}

function toolName(tool: ToolEntry): string {
  return typeof tool === "string" ? tool : tool.name;
}

export default function McpPage() {
  const setup = useRead<McpSetup>("mcp_setup", undefined, NO_WORKSPACE);
  const endpoint = setup.data?.mcp_url ?? "";
  return (
    <Page
      eyebrow="Developer"
      title="MCP"
      sub={
        <>
          Connect an agent to lake. Every capability on this site is an MCP tool at <code>{endpoint || "…/mcp"}</code>, the same registry REST and OpenAPI serve,
          checked and audited as your own calls.
        </>
      }
    >
      <Query q={setup} what="the MCP setup" lines={6}>
        {(value) => <McpBody setup={value} />}
      </Query>
    </Page>
  );
}

function McpBody({ setup }: { setup: McpSetup }) {
  const [access, setAccess] = useState<Access>("all");
  const [lifetime, setLifetime] = useState("");
  const [reason, setReason] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [key, setKey] = useState<string | null>(null);
  const readOnly = new Set(setup.scopes.read_only);

  const create = async () => {
    setBusy(true);
    setError(null);
    try {
      const value = await act<{ secret?: string }>("create_token", mcpKeyInput(access, lifetime, reason, setup.scopes), NO_WORKSPACE);
      setKey(value.secret ?? null);
    } catch (e) {
      setError(messageOf(e));
    } finally {
      setBusy(false);
    }
  };

  const endpoint = setup.mcp_url;
  const bearer = key ?? "$LAKE_KEY";
  const claudeCode = `claude mcp add --transport http lake ${endpoint} --header "Authorization: Bearer ${bearer}"`;
  const cursor = `{\n  "mcpServers": {\n    "lake": {\n      "url": "${endpoint}",\n      "headers": { "Authorization": "Bearer ${bearer}" }\n    }\n  }\n}`;
  const desktop = `{\n  "mcpServers": {\n    "lake": {\n      "command": "npx",\n      "args": ["-y", "mcp-remote", "${endpoint}", "--header", "Authorization: Bearer ${bearer}"]\n    }\n  }\n}`;
  const curl =
    `curl -s ${endpoint} -H "Authorization: Bearer ${bearer}" -H 'Content-Type: application/json' \\\n` +
    `  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"curl","version":"1"}}}'\n` +
    `curl -s ${endpoint} -H "Authorization: Bearer ${bearer}" -H 'Content-Type: application/json' \\\n` +
    `  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'`;

  return (
    <>
      {error && (
        <div className="panel notice status-danger" role="alert">
          <p>{error}</p>
        </div>
      )}
      {key ? (
        <section className="panel notice" role="status">
          <h2>Your MCP Key</h2>
          <p>
            Shown once. It is in every block below; copy what you need now. Revoke it on <Link to="/tokens">Tokens</Link>.
          </p>
          <CopyBlock value={key} label="Key" />
        </section>
      ) : (
        <section className="panel">
          <h2>Create an MCP Key</h2>
          <p>
            A lake API token for your agent or script. It acts as you. By default it lasts as long as your sign-in, at most 30 days; choose <em>Never</em> for a
            standing key, if your space allows keys that never expire. Revoke any key on <Link to="/tokens">Tokens</Link>.
          </p>
          <Form onSubmit={create}>
            <fieldset>
              <legend>Access</legend>
              <label>
                <input type="radio" name="access" value="all" checked={access === "all"} onChange={() => setAccess("all")} /> Every tool
              </label>
              <label>
                <input type="radio" name="access" value="read" checked={access === "read"} onChange={() => setAccess("read")} /> Read-only tools
              </label>
            </fieldset>
            <Field label="Lifetime">
              <select name="expires_in_seconds" value={lifetime} onChange={(e) => setLifetime(e.target.value)}>
                <option value="">As long as my sign-in allows (at most 30 days)</option>
                <option value="86400">1 day</option>
                <option value="604800">7 days</option>
                <option value="never">Never (a standing key)</option>
              </select>
            </Field>
            <Field label="Why this key exists" help="Needed for a key that never expires.">
              <input name="reason" placeholder="the nightly sync from our data warehouse" value={reason} onChange={(e) => setReason(e.target.value)} />
            </Field>
            <div>
              <Button type="submit" variant="primary" loading={busy}>
                Create an MCP key
              </Button>
            </div>
          </Form>
        </section>
      )}
      <section className="panel">
        <h2>Set Up</h2>
        {!key && (
          <p className="muted">
            Create a key above, or put yours in <code>$LAKE_KEY</code>.
          </p>
        )}
        <CopyBlock label="Claude Code" value={claudeCode} />
        <CopyBlock label="Cursor (mcp.json)" value={cursor} />
        <CopyBlock label="Claude Desktop (claude_desktop_config.json)" value={desktop} />
        <CopyBlock label="Any client: JSON-RPC over HTTP" value={curl} />
        <h3 id="mcp-upgrades">After lake Upgrades</h3>
        <p>
          Reconnect your MCP client to lake, or restart it, after lake is upgraded. lake tells a connected client when its tools change (
          <code>notifications/tools/list_changed</code>), but a client connected before an upgrade, or one that ignores the notice, keeps the tools and schemas it
          first listed.
        </p>
      </section>
      <section className="panel">
        <h2>Teach Your Agent</h2>
        <ul>
          <li>
            <a href="/skills/lake/SKILL.md" download="SKILL.md">
              Download the lake skill
            </a>
            : a Claude skill (<code>SKILL.md</code>) with the setup, which tools to use for what, the flows below and the rules. Put it in{" "}
            <code>~/.claude/skills/lake/</code>.
          </li>
          <li>
            <a href="/llms.txt">llms.txt</a>: lake in one page for any agent.
          </li>
          <li>
            <a href="/llms-full.txt">llms-full.txt</a>: every tool with its input schema.
          </li>
          <li>
            <a href="/v1/openapi.json">OpenAPI</a>: the same tools as REST, <code>GET /v1/&lt;tool&gt;</code> for a read and <code>POST /v1/&lt;tool&gt;</code> for
            an action.
          </li>
        </ul>
      </section>
      <section className="panel">
        <h2>Worked Flows</h2>
        {setup.flows.map((flow) => (
          <div key={flow.title}>
            <h3>{flow.title}</h3>
            {flow.summary && <p>{flow.summary}</p>}
            <ol>
              {flow.steps.map((step, index) => (
                <li key={index}>
                  {typeof step === "string" ? (
                    step
                  ) : (
                    <>
                      <code>{step.tool}</code>
                      {step.note ? `: ${step.note}` : null}
                    </>
                  )}
                </li>
              ))}
            </ol>
          </div>
        ))}
      </section>
      <section className="panel">
        <h2>What the Tools Do</h2>
        {setup.tool_groups.map((group) => (
          <details key={group.title}>
            <summary>
              {group.title} <span className="muted">{group.tools.length} tools</span>
            </summary>
            {group.summary && <p>{group.summary}</p>}
            <div className="table-wrap">
              <table>
                <thead>
                  <tr>
                    <th>Tool</th>
                    <th>What it does</th>
                    <th>Access</th>
                  </tr>
                </thead>
                <tbody>
                  {group.tools.map((tool) => {
                    const name = toolName(tool);
                    const what = typeof tool === "string" ? null : (tool.title ?? tool.description ?? null);
                    const writes = typeof tool !== "string" && tool.writes !== undefined ? tool.writes : !readOnly.has(name);
                    return (
                      <tr key={name}>
                        <td>
                          <code>{name}</code>
                        </td>
                        <td>{what ?? <span className="muted">—</span>}</td>
                        <td>{writes ? "write" : "read"}</td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          </details>
        ))}
      </section>
    </>
  );
}
