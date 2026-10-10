/** Developer → MCP (`/developer/mcp`): connect an agent to lake, after rev-os's Developer → MCP.
 * The endpoint, the tool groups, the worked flows and the read-only scopes come from `mcp_setup`;
 * a key is `create_token` (the Keys tab, shared with Developer → API). Connect has the setup for
 * each client with the new key filled in, or one pasted, or `$LAKE_KEY`; the skill, `llms.txt`
 * and OpenAPI. Tools lists every tool `tools/list` serves, by group, read or write, and the flows.
 * The key is shown once and lives only in this page's memory. */
import { type ReactNode, useMemo, useState } from "react";
import { ArrowUpRight, ChevronDown, ChevronRight, Code, KeyRound, Lock, PenLine, Search, Sparkles } from "lucide-react";
import { useSession } from "@/lib/session";
import { Button, Card, cx, Empty, Page, Query, StateTabs } from "@/lib/ui";
import { CodeBlock, Chip, DownloadButton, KeysPanel, type McpSetup, originOf, PageTabs, useMcpSetup, useTab } from "./parts";

const TABS = ["connect", "keys", "tools"] as const;
type Tab = (typeof TABS)[number];
type Client = "claude-code" | "cursor" | "claude-desktop" | "curl";

export default function McpPage() {
  const tab = useTab(TABS);
  const { viewer } = useSession();
  const setup = useMcpSetup();
  const [fresh, setFresh] = useState<string | null>(null);
  const [pasted, setPasted] = useState("");
  const endpoint = setup.data?.mcp_url ?? `${originOf(viewer)}/mcp`;
  const toolCount = setup.data?.tool_groups.reduce((n, g) => n + g.tools.length, 0);

  return (
    <Page
      eyebrow="Developer"
      title="MCP"
      sub={
        <>
          Connect Claude Code, Cursor, Claude Desktop or any MCP client to lake. Every capability on this site is an MCP tool at <code>{endpoint}</code>, from the
          same registry REST and OpenAPI serve, checked and audited as your own calls.
        </>
      }
      actions={
        <Button to="/developer/api">
          <Code size={14} aria-hidden /> API keys and operations
        </Button>
      }
    >
      <div className="mb-5 flex flex-wrap items-center gap-2 rounded-[7px] border border-hairline bg-surface px-4 py-2.5 text-[12.5px]">
        <span className="label m-0">Endpoint</span>
        <code className="font-mono break-all text-ink">{endpoint}</code>
        <Chip>JSON-RPC 2.0</Chip>
        <Chip>protocol 2025-06-18</Chip>
        <Chip tone="action">Authorization: Bearer &lt;key&gt;</Chip>
      </div>
      <PageTabs<Tab>
        value={tab}
        tabs={[
          { id: "connect", label: "Connect" },
          { id: "keys", label: "Keys" },
          { id: "tools", label: "Tools", count: toolCount },
        ]}
      />
      {tab === "connect" && <Connect endpoint={endpoint} fresh={fresh} pasted={pasted} onPaste={setPasted} admin={viewer.administrator} />}
      {tab === "keys" && <KeysPanel purpose="mcp" setup={setup} fresh={fresh} onCreated={setFresh} onDone={() => setFresh(null)} />}
      {tab === "tools" && (
        <Query q={setup} what="the MCP setup" lines={8}>
          {(value) => <Tools setup={value} />}
        </Query>
      )}
    </Page>
  );
}

function Connect({ endpoint, fresh, pasted, onPaste, admin }: { endpoint: string; fresh: string | null; pasted: string; onPaste: (v: string) => void; admin: boolean }) {
  const [client, setClient] = useState<Client>("claude-code");
  const key = fresh || pasted.trim() || "$LAKE_KEY";
  const real = !!(fresh || pasted.trim());
  return (
    <div className="space-y-6">
      <Step n={1} title="Your Key" aside={admin ? "The operator connects with the admin token." : "Make one on the Keys tab, or paste one you have. It stays in this page only."}>
        <Card>
          {fresh ? (
            <p className="m-0 flex items-center gap-2 text-[13px] text-ok">
              <KeyRound size={14} aria-hidden /> Your new key is filled into every block below.
            </p>
          ) : (
            <div className="space-y-2">
              <p className="m-0 text-[13px] text-ink-2">
                {admin ? (
                  <>Paste the admin token to fill the blocks, or leave them reading <code>$LAKE_KEY</code>.</>
                ) : (
                  <>
                    <Button to="?tab=keys" variant="primary">
                      <KeyRound size={14} aria-hidden /> Create an MCP key
                    </Button>{" "}
                    or paste a key you already have:
                  </>
                )}
              </p>
              <input
                type="password"
                autoComplete="off"
                aria-label="A key you already have"
                value={pasted}
                onChange={(e) => onPaste(e.target.value)}
                placeholder="Paste a key to fill the setup blocks"
                className="w-full max-w-md"
              />
            </div>
          )}
        </Card>
      </Step>
      <Step n={2} title="Set Up Your Client" aside={real ? "Your key is in every block. Copy the one for your client." : <>The blocks read the key from <code>$LAKE_KEY</code> until you make or paste one.</>}>
        <StateTabs<Client>
          value={client}
          onChange={setClient}
          label="Client"
          tabs={[
            { value: "claude-code", label: "Claude Code" },
            { value: "cursor", label: "Cursor" },
            { value: "claude-desktop", label: "Claude Desktop" },
            { value: "curl", label: "Any client (curl)" },
          ]}
        />
        <SetupBlock client={client} endpoint={endpoint} keyValue={key} />
        <h3 id="mcp-upgrades">After Lake Upgrades</h3>
        <p className="m-0 text-[13px] text-ink-2">
          Reconnect your MCP client to lake, or restart it, after lake is upgraded. Lake tells a connected client when its tools change (
          <code>notifications/tools/list_changed</code>), but a client connected before an upgrade, or one that ignores the notice, keeps the tools and schemas it
          first listed.
        </p>
      </Step>
      <Step n={3} title="Teach Your Agent" aside="The skill tells an agent what lake is, which tools to use for what, the worked flows and the rules.">
        <Card>
          <div className="flex flex-wrap items-center gap-2">
            <DownloadButton path="/skills/lake/SKILL.md" filename="SKILL.md">
              Download the lake skill
            </DownloadButton>
            <DownloadButton path="/llms-full.txt" filename="lake-llms-full.txt">
              llms-full.txt
            </DownloadButton>
            <Button href="/llms.txt">
              llms.txt <ArrowUpRight size={13} aria-hidden />
            </Button>
            <Button to="/developer/api?tab=operations">OpenAPI operations</Button>
          </div>
          <ul className="mt-3 mb-0 space-y-1 pl-5 text-[12.5px] text-ink-2">
            <li>
              <b className="text-ink">The lake skill</b>: a Claude skill (<code>SKILL.md</code>) with the setup, the tools for each job, the flows and the rules. Put
              it in <code>~/.claude/skills/lake/</code>.
            </li>
            <li>
              <b className="text-ink">llms.txt</b>: lake in one page for any agent; <b className="text-ink">llms-full.txt</b> adds every tool with its input schema.
            </li>
            <li>
              <b className="text-ink">OpenAPI</b>: the same tools as REST, <code>GET /v1/&lt;tool&gt;</code> for a read and <code>POST /v1/&lt;tool&gt;</code> for an
              action.
            </li>
          </ul>
          <div className="mt-3">
            <CodeBlock title="install the skill for Claude Code" code={`mkdir -p ~/.claude/skills/lake && mv ~/Downloads/SKILL.md ~/.claude/skills/lake/SKILL.md`} />
          </div>
        </Card>
      </Step>
    </div>
  );
}

/** One setup block per client, the key filled in. The ids are the ones the browser check reads. */
function SetupBlock({ client, endpoint, keyValue }: { client: Client; endpoint: string; keyValue: string }) {
  if (client === "claude-code")
    return (
      <CodeBlock
        id="mcp-claude-code"
        title="terminal"
        code={`claude mcp add --transport http lake ${endpoint} --header "Authorization: Bearer ${keyValue}"`}
        note="Then run /mcp inside Claude Code to see lake connected. Add --scope user to use it in every project."
      />
    );
  if (client === "cursor")
    return (
      <CodeBlock
        id="mcp-cursor"
        title="~/.cursor/mcp.json"
        code={JSON.stringify({ mcpServers: { lake: { url: endpoint, headers: { Authorization: `Bearer ${keyValue}` } } } }, null, 2)}
        note="Cursor → Settings → MCP shows lake and its tools once the file is saved."
      />
    );
  if (client === "claude-desktop")
    return (
      <CodeBlock
        id="mcp-claude-desktop"
        title="claude_desktop_config.json"
        code={JSON.stringify({ mcpServers: { lake: { command: "npx", args: ["-y", "mcp-remote", endpoint, "--header", `Authorization: Bearer ${keyValue}`] } } }, null, 2)}
        note="Claude Desktop → Settings → Developer → Edit Config. mcp-remote bridges the desktop app to lake over HTTP. Restart Claude Desktop afterwards."
      />
    );
  return (
    <div className="space-y-3">
      <CodeBlock
        id="mcp-curl-initialize"
        title="initialize"
        code={`curl -s ${endpoint} \\\n  -H "Authorization: Bearer ${keyValue}" \\\n  -H 'Content-Type: application/json' \\\n  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"curl","version":"1"}}}'`}
      />
      <CodeBlock
        id="mcp-curl-tools"
        title="tools/list"
        code={`curl -s ${endpoint} \\\n  -H "Authorization: Bearer ${keyValue}" \\\n  -H 'Content-Type: application/json' \\\n  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'`}
        note="tools/list needs no key; tools/call on anything but server_info does."
      />
      <CodeBlock
        id="mcp-curl-call"
        title="tools/call"
        code={`curl -s ${endpoint} \\\n  -H "Authorization: Bearer ${keyValue}" \\\n  -H 'Content-Type: application/json' \\\n  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"list_customers","arguments":{"limit":5}}}'`}
        note="A refused call is a result with isError: true and the same error envelope REST answers."
      />
    </div>
  );
}

function Tools({ setup }: { setup: McpSetup }) {
  const [needle, setNeedle] = useState("");
  const [access, setAccess] = useState<"all" | "read" | "write">("all");
  const [open, setOpen] = useState<string | null>(null);
  const all = setup.tool_groups.flatMap((g) => g.tools);
  const writes = all.filter((t) => t.writes).length;
  const groups = useMemo(() => {
    const q = needle.trim().toLowerCase();
    return setup.tool_groups
      .map((group) => ({
        ...group,
        tools: group.tools.filter(
          (t) => (access === "all" || (access === "write") === t.writes) && (!q || `${t.name} ${t.title} ${group.title}`.toLowerCase().includes(q)),
        ),
      }))
      .filter((group) => group.tools.length > 0);
  }, [setup, needle, access]);

  return (
    <div className="space-y-6">
      <div>
        <div className="mb-3 flex flex-wrap items-center gap-3">
          <label className="relative flex w-full max-w-xs items-center">
            <Search size={14} className="pointer-events-none absolute left-2.5 text-ink-3" aria-hidden />
            <span className="sr-only">Find a tool</span>
            <input type="search" value={needle} onChange={(e) => setNeedle(e.target.value)} placeholder="Find a tool" className="w-full pl-8" />
          </label>
          <StateTabs<"all" | "read" | "write">
            value={access}
            onChange={setAccess}
            label="Access"
            tabs={[
              { value: "all", label: `All · ${all.length}` },
              { value: "read", label: `Read · ${all.length - writes}` },
              { value: "write", label: `Write · ${writes}` },
            ]}
          />
        </div>
        {!groups.length ? (
          <Empty>{all.length ? "No tool matches." : "No tools are listed."}</Empty>
        ) : (
          <div className="space-y-3">
            {groups.map((group) => {
              const isOpen = open === group.title || !!needle.trim();
              return (
                <section key={group.title} className="overflow-hidden rounded-[6px] border border-hairline bg-surface">
                  <button
                    type="button"
                    aria-expanded={isOpen}
                    onClick={() => setOpen(open === group.title ? null : group.title)}
                    className="flex min-h-0 w-full cursor-pointer items-start justify-start gap-3 border-0 bg-transparent px-4 py-2.5 text-left hover:bg-raised"
                  >
                    {isOpen ? <ChevronDown size={14} className="mt-[3px] shrink-0 text-ink-3" aria-hidden /> : <ChevronRight size={14} className="mt-[3px] shrink-0 text-ink-3" aria-hidden />}
                    <span className="min-w-0">
                      <span className="block text-[13.5px] font-semibold text-ink">
                        {group.title} <span className="font-mono text-[11px] font-normal text-ink-3">{group.tools.length}</span>
                      </span>
                      {group.summary && <span className="block text-[12.5px] text-ink-2">{group.summary}</span>}
                    </span>
                  </button>
                  {isOpen && (
                    <ul className="m-0 list-none divide-y divide-hairline border-t border-hairline p-0">
                      {group.tools.map((tool) => (
                        <li key={tool.name} className="flex items-center gap-3 px-4 py-1.5">
                          {tool.writes ? (
                            <span className="inline-flex w-[52px] shrink-0 items-center gap-1 font-mono text-[10.5px] text-action">
                              <PenLine size={10} aria-hidden /> write
                            </span>
                          ) : (
                            <span className="inline-flex w-[52px] shrink-0 items-center gap-1 font-mono text-[10.5px] text-ok">
                              <Lock size={10} aria-hidden /> read
                            </span>
                          )}
                          <code className="min-w-0 border-0 bg-transparent p-0 font-mono text-[12.5px] break-all text-ink">{tool.name}</code>
                          <span className="ml-auto hidden max-w-[55%] truncate text-[12px] text-ink-3 md:inline">{tool.title}</span>
                        </li>
                      ))}
                    </ul>
                  )}
                </section>
              );
            })}
          </div>
        )}
        <p className="mt-3 flex items-center gap-1.5 text-[12px] text-ink-3">
          <Sparkles size={12} aria-hidden /> The same tools <code>tools/list</code> serves; a read-only key sees every tool but is refused the writes. Each call runs
          as you and is recorded in the audit log.
        </p>
      </div>
      <section>
        <h2>Worked Flows</h2>
        <div className="grid grid-cols-1 gap-3 lg:grid-cols-2">
          {setup.flows.map((flow) => (
            <Card key={flow.title} title={flow.title}>
              {flow.summary && <p className="text-[12.5px] text-ink-2">{flow.summary}</p>}
              <ol className="m-0 space-y-1 pl-5 text-[12.5px]">
                {flow.steps.map((step, index) => (
                  <li key={index}>
                    <code className="font-mono text-ink">{step.tool}</code>
                    {step.note && <span className="text-ink-2">: {step.note}</span>}
                  </li>
                ))}
              </ol>
            </Card>
          ))}
        </div>
      </section>
    </div>
  );
}

/** A numbered step with its title and a line of advice on the left (rev-os's setup steps). */
function Step({ n, title, aside, children }: { n: number; title: string; aside?: ReactNode; children: ReactNode }) {
  return (
    <section className="grid grid-cols-1 gap-4 border-t border-hairline pt-5 first:border-t-0 first:pt-0 lg:grid-cols-[220px_minmax(0,1fr)]">
      <div>
        <div className="flex items-center gap-2.5">
          <span className="flex h-6 w-6 shrink-0 items-center justify-center rounded-full bg-action font-mono text-[11px] font-semibold text-on-action">{n}</span>
          <h3 className="m-0 text-[14.5px]">{title}</h3>
        </div>
        {aside && <div className={cx("mt-2 pl-[34px] text-[12.5px] leading-relaxed text-ink-3")}>{aside}</div>}
      </div>
      <div className="min-w-0">{children}</div>
    </section>
  );
}
