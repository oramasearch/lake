/** Developer → MCP (`/developer/mcp`): connect an agent to lake on one page, after rev-os's
 * Developer → MCP. The endpoint, the tool groups, the worked flows and the read-only scopes come
 * from `mcp_setup`; a key is `create_token`. Six numbered steps: make a key (shown once) or paste
 * one; pick a client and copy its exact setup with the key filled in (`clientSetup` in logic.ts);
 * teach the agent with the lake skill; what the tools do, filtered by group and by read or write;
 * the worked flows; and the viewer's keys. The key lives only in this page's memory. `?tab=keys`
 * and `?tab=tools`, the addresses of the former tabs, scroll to their step. */
import { type ComponentType, useEffect, useMemo, useState } from "react";
import { useSearchParams } from "react-router-dom";
import {
  ArrowUpRight,
  Braces,
  ChevronDown,
  CircleCheck,
  Code,
  CodeXml,
  FileCode,
  Gem,
  Globe,
  Info,
  KeyRound,
  Lock,
  Monitor,
  MousePointer2,
  PenLine,
  Search,
  Sparkles,
  SquareTerminal,
  Terminal,
  Wind,
} from "lucide-react";
import { useSession } from "@/lib/session";
import { Button, Card, Chip as Pill, cx, Empty, FilterChips, Page, Query } from "@/lib/ui";
import { CLIENTS, type ClientId, clientSetup, KEY_ENV, SKILL_TARGETS, type SkillTarget, skillInstall } from "./logic";
import {
  Chip,
  CodeBlock,
  CopyButton,
  DownloadButton,
  EndpointStrip,
  KeyCreator,
  KeysTable,
  type McpSetup,
  NewKeyCallout,
  OperatorKeys,
  originOf,
  Step,
  useMcpSetup,
} from "./parts";

const ICONS: Record<ClientId, ComponentType<{ size?: number; className?: string; "aria-hidden"?: boolean }>> = {
  "claude-code": Terminal,
  codex: SquareTerminal,
  "claude-desktop": Monitor,
  cursor: MousePointer2,
  vscode: CodeXml,
  windsurf: Wind,
  gemini: Gem,
  curl: Braces,
  web: Globe,
};

/** The skill install that matches the client picked in step 2. */
const SKILL_FOR: Partial<Record<ClientId, SkillTarget>> = { "claude-code": "claude-code", codex: "codex", gemini: "gemini" };

export default function McpPage() {
  const { viewer } = useSession();
  const setup = useMcpSetup();
  const [params] = useSearchParams();
  const [fresh, setFresh] = useState<string | null>(null);
  const [pasted, setPasted] = useState("");
  const [client, setClient] = useState<ClientId>("claude-code");
  const origin = originOf(viewer);
  const endpoint = setup.data?.mcp_url ?? `${origin}/mcp`;
  const toolCount = setup.data?.tool_groups.reduce((n, g) => n + g.tools.length, 0);
  const key = fresh || pasted.trim() || null;
  const admin = viewer.administrator;
  const asked = params.get("tab");

  // The former tabs' addresses open their step.
  useEffect(() => {
    if (asked !== "keys" && asked !== "tools") return;
    const at = document.getElementById(asked === "keys" ? "keys" : "tools");
    at?.scrollIntoView({ block: "start" });
  }, [asked, setup.isSuccess]);

  return (
    <Page
      eyebrow="Developer"
      title="MCP"
      sub={
        <>
          Connect Claude Code, Codex, Cursor, Claude Desktop or any MCP client to Lake. The agent at the other end acts as you, with your permissions, and can do
          nothing your key does not allow. Every call is checked and audited as your own.
        </>
      }
      actions={
        <Button to="/developer/api">
          <Code size={14} aria-hidden /> API keys and operations
        </Button>
      }
    >
      <EndpointStrip label="Endpoint" value={endpoint}>
        <Chip>JSON-RPC 2.0</Chip>
        <Chip>streamable HTTP</Chip>
        <Chip>protocol 2025-06-18</Chip>
        <Chip tone="action">Authorization: Bearer &lt;key&gt;</Chip>
        {toolCount !== undefined && <Chip tone="ok">{toolCount} tools</Chip>}
      </EndpointStrip>

      <div className="space-y-4">
        <Step
          n={1}
          title="Create an MCP Key"
          aside={
            admin
              ? "The operator connects with the instance's admin token."
              : "Read-only is the right default for an agent that reports. Choose every tool only for one that should also act, still within your permissions."
          }
        >
          {admin ? (
            <>
              <OperatorKeys />
              <PasteKey value={pasted} onChange={setPasted} open />
            </>
          ) : fresh ? (
            <NewKeyCallout value={fresh} onDone={() => setFresh(null)} purpose="mcp" />
          ) : (
            <>
              <Card>
                <Query q={setup} what="the key options" lines={4}>
                  {(value) => <KeyCreator purpose="mcp" scopes={value.scopes} onCreated={setFresh} />}
                </Query>
              </Card>
              <PasteKey value={pasted} onChange={setPasted} />
            </>
          )}
        </Step>

        <Step
          n={2}
          title="Set Up Your Client"
          aside={
            key ? (
              "Your key is filled into every block. Pick your client and copy its setup."
            ) : (
              <>
                The blocks read the key from <code>${KEY_ENV}</code> until you make or paste one in step 1.
              </>
            )
          }
        >
          <ClientPicker value={client} onChange={setClient} />
          <ClientPanel client={client} endpoint={endpoint} keyValue={key} />
          <p id="mcp-upgrades" className="m-0 flex gap-2 text-[12.5px] leading-relaxed text-ink-3">
            <Info size={13} className="mt-[3px] shrink-0" aria-hidden />
            <span>
              <b className="font-semibold text-ink-2">After Lake upgrades</b>, reconnect or restart your client. Lake tells a connected client when its tools change (
              <code>notifications/tools/list_changed</code>), but a client connected before an upgrade, or one that ignores the notice, keeps the tools it first listed.
            </span>
          </p>
        </Step>

        <Step n={3} title="Teach Your Agent" aside="The skill tells an agent what Lake is, which tool to use for what, the worked flows and the rules it must keep.">
          <TeachAgent key={client} origin={origin} client={client} />
        </Step>

        <Step
          n={4}
          id="tools"
          title="What the Tools Do"
          aside="One tool per capability, in Lake's product groups: the same list tools/list serves. A read-only key sees every tool and is refused the writes."
        >
          <Query q={setup} what="the MCP tools" lines={8}>
            {(value) => <Tools setup={value} />}
          </Query>
        </Step>

        <Step n={5} title="Worked Flows" aside="The tools an agent calls, in order, for the jobs people ask of Lake most.">
          <Query q={setup} what="the worked flows" lines={4}>
            {(value) => <Flows setup={value} />}
          </Query>
        </Step>

        <Step n={6} id="keys" title="Your Keys" aside="A key works for both MCP and the REST API. Revoking one stops it at once, and anything using it.">
          {admin ? <p className="m-0 text-[12.5px] text-ink-3">The operator has no keys of its own.</p> : <KeysTable scopes={setup.data?.scopes} />}
        </Step>
      </div>
    </Page>
  );
}

function PasteKey({ value, onChange, open }: { value: string; onChange: (v: string) => void; open?: boolean }) {
  return (
    <details open={open || !!value} className="rounded-[6px] border border-hairline bg-surface px-4 py-2.5 text-[12.5px]">
      <summary className="flex cursor-pointer items-center gap-1.5 text-ink-2 select-none">
        <KeyRound size={13} aria-hidden /> Already have a key? Paste it to fill the setup blocks. It stays in this page only.
      </summary>
      <input
        type="password"
        autoComplete="off"
        aria-label="A key you already have"
        value={value}
        onChange={(e) => onChange(e.target.value)}
        placeholder="Paste a key to fill the setup blocks"
        className="mt-2 w-full max-w-md"
      />
    </details>
  );
}

/** The row of client tiles: an icon, the name and how it connects. */
function ClientPicker({ value, onChange }: { value: ClientId; onChange: (v: ClientId) => void }) {
  return (
    <div role="group" aria-label="Client" className="grid grid-cols-1 gap-2 sm:grid-cols-2 xl:grid-cols-3">
      {CLIENTS.map((c) => {
        const Icon = ICONS[c.id];
        const on = c.id === value;
        return (
          <button
            key={c.id}
            type="button"
            aria-pressed={on}
            data-client={c.id}
            onClick={() => onChange(c.id)}
            className={cx(
              "flex h-auto min-h-0 w-full cursor-pointer items-start justify-start gap-3 rounded-[7px] border px-3 py-2.5 text-left font-normal whitespace-normal transition-colors",
              on ? "border-action bg-action-soft" : "border-hairline bg-surface hover:border-rule hover:bg-raised",
            )}
          >
            <span
              className={cx(
                "flex h-8 w-8 shrink-0 items-center justify-center rounded-[6px] border",
                on ? "border-action bg-action text-on-action" : "border-hairline bg-raised text-ink-2",
              )}
            >
              <Icon size={16} aria-hidden />
            </span>
            <span className="min-w-0">
              <span className={cx("block text-[13px] font-semibold", on ? "text-action" : "text-ink")}>{c.name}</span>
              <span className="block text-[12px] leading-snug text-ink-3">{c.how}</span>
            </span>
          </button>
        );
      })}
    </div>
  );
}

/** The chosen client's setup: where it goes, the blocks, how to check, and what to know. */
function ClientPanel({ client, endpoint, keyValue }: { client: ClientId; endpoint: string; keyValue: string | null }) {
  const setup = clientSetup(client, endpoint, keyValue);
  const name = CLIENTS.find((c) => c.id === client)?.name ?? client;
  return (
    <div className="space-y-3 rounded-[8px] border border-hairline bg-raised p-3 sm:p-4" aria-live="polite">
      <div className="flex flex-wrap items-baseline gap-x-3 gap-y-1">
        <span className="text-[13.5px] font-semibold text-ink">{name}</span>
        {setup.where.map((w) => (
          <span key={w.path} className="inline-flex max-w-full min-w-0 flex-wrap items-center gap-x-1.5 gap-y-0.5 text-[12px] text-ink-3">
            <FileCode size={12} className="shrink-0" aria-hidden />
            <span className="whitespace-nowrap">{w.label}</span>
            <code className="font-mono text-[11.5px] [overflow-wrap:anywhere] text-ink-2">{w.path}</code>
            <CopyButton text={w.path} label="Copy path" className="border-hairline bg-surface text-ink-3 hover:text-ink" />
          </span>
        ))}
      </div>
      {setup.blocks.map((block) => (
        <CodeBlock key={block.id} id={block.id} title={block.title} code={block.code} note={block.note} />
      ))}
      <p className="m-0 flex gap-2 text-[12.5px] leading-relaxed text-ink-2">
        <CircleCheck size={14} className="mt-[2px] shrink-0 text-ok" aria-hidden />
        <span>
          <b className="font-semibold text-ink">Check it worked.</b> {setup.check}
        </span>
      </p>
      {setup.notes.map((note) => (
        <p key={note} className="m-0 flex gap-2 text-[12.5px] leading-relaxed text-ink-2">
          <Info size={14} className="mt-[2px] shrink-0 text-ink-3" aria-hidden />
          <span>{note}</span>
        </p>
      ))}
    </div>
  );
}

function TeachAgent({ origin, client }: { origin: string; client: ClientId }) {
  // Keyed by the client in step 2, so it opens on that client's install.
  const [target, setTarget] = useState<SkillTarget>(SKILL_FOR[client] ?? "other");
  return (
    <Card>
      <div className="flex flex-wrap items-center gap-2">
        <DownloadButton path="/skills/lake/SKILL.md" filename="SKILL.md">
          Download the Lake skill
        </DownloadButton>
        <DownloadButton path="/llms-full.txt" filename="lake-llms-full.txt">
          llms-full.txt
        </DownloadButton>
        <Button href="/llms.txt">
          llms.txt <ArrowUpRight size={13} aria-hidden />
        </Button>
        <Button to="/developer/api?tab=operations">OpenAPI operations</Button>
      </div>
      <ul className="mt-3 mb-4 space-y-1 pl-5 text-[12.5px] text-ink-2">
        <li>
          <b className="text-ink">The Lake skill</b>: a <code>SKILL.md</code> with the setup, the tools for each job, the flows and the rules. It is public, so the
          commands below fetch it straight from Lake.
        </li>
        <li>
          <b className="text-ink">llms.txt</b>: Lake in one page for any agent; <b className="text-ink">llms-full.txt</b> adds every tool with its input schema.
        </li>
        <li>
          <b className="text-ink">OpenAPI</b>: the same tools as REST, <code>GET /v1/&lt;tool&gt;</code> for a read and <code>POST /v1/&lt;tool&gt;</code> for an action.
        </li>
      </ul>
      <div className="mb-3 flex flex-wrap items-center gap-1.5" role="group" aria-label="Agent">
        <span className="label m-0 mr-1">Install for</span>
        {SKILL_TARGETS.map((t) => (
          <Pill key={t.id} onClick={() => setTarget(t.id)} active={target === t.id}>
            {t.label}
          </Pill>
        ))}
      </div>
      <div className="space-y-3">
        {skillInstall(target, origin).map((block) => (
          <CodeBlock key={block.id} id={block.id} title={block.title} code={block.code} note={block.note} />
        ))}
      </div>
    </Card>
  );
}

const PREVIEW = 6;

function Tools({ setup }: { setup: McpSetup }) {
  const [needle, setNeedle] = useState("");
  const [access, setAccess] = useState<"read" | "write" | null>(null);
  const [group, setGroup] = useState<string | null>(null);
  const [expanded, setExpanded] = useState<Set<string>>(new Set());
  const all = setup.tool_groups.flatMap((g) => g.tools);
  const writes = all.filter((t) => t.writes).length;
  const q = needle.trim().toLowerCase();
  const groups = useMemo(
    () =>
      setup.tool_groups
        .filter((g) => !group || g.title === group)
        .map((g) => ({
          ...g,
          tools: g.tools.filter((t) => (!access || (access === "write") === t.writes) && (!q || `${t.name} ${t.title} ${g.title}`.toLowerCase().includes(q))),
        }))
        .filter((g) => g.tools.length > 0),
    [setup, group, access, q],
  );
  const shown = groups.reduce((n, g) => n + g.tools.length, 0);
  const filtered = !!(q || access || group);

  return (
    <div>
      <div className="mb-3 flex flex-wrap items-center gap-3">
        <label className="relative flex w-full max-w-xs items-center">
          <Search size={14} className="pointer-events-none absolute left-2.5 text-ink-3" aria-hidden />
          <span className="sr-only">Find a tool</span>
          <input type="search" value={needle} onChange={(e) => setNeedle(e.target.value)} placeholder="Find a tool" className="w-full pl-8" />
        </label>
        <FilterChips<"read" | "write">
          value={access}
          onChange={setAccess}
          allLabel={`All ${all.length}`}
          options={[
            { id: "read", label: "read", count: all.length - writes },
            { id: "write", label: "write", count: writes },
          ]}
        />
        <span className="font-mono text-[12px] text-ink-3 tabular-nums sm:ml-auto">{filtered ? `${shown} of ${all.length}` : all.length} tools</span>
      </div>
      <div className="mb-4 flex flex-wrap items-center gap-1.5" role="group" aria-label="Tool group">
        <Pill onClick={() => setGroup(null)} active={group === null}>
          Every group
        </Pill>
        {setup.tool_groups.map((g) => (
          <Pill key={g.title} onClick={() => setGroup(group === g.title ? null : g.title)} active={group === g.title}>
            {g.title}
            <span className="font-mono text-[10.5px] text-ink-3">{g.tools.length}</span>
          </Pill>
        ))}
      </div>
      {!groups.length ? (
        <Empty>{all.length ? "No tool matches." : "No tools are listed."}</Empty>
      ) : (
        <div className="space-y-3">
          {groups.map((g) => {
            const whole = filtered || expanded.has(g.title) || g.tools.length <= PREVIEW + 2;
            const tools = whole ? g.tools : g.tools.slice(0, PREVIEW);
            const groupWrites = g.tools.filter((t) => t.writes).length;
            return (
              <section key={g.title} className="overflow-hidden rounded-[6px] border border-hairline bg-surface">
                <div className="border-b border-hairline px-4 py-2.5">
                  <div className="flex flex-wrap items-center gap-2">
                    <span className="text-[13.5px] font-semibold text-ink">{g.title}</span>
                    <span className="font-mono text-[11px] text-ink-3">
                      {g.tools.length} · read {g.tools.length - groupWrites} · write {groupWrites}
                    </span>
                  </div>
                  {g.summary && <div className="text-[12.5px] text-ink-2">{g.summary}</div>}
                </div>
                <ul className="m-0 list-none divide-y divide-hairline p-0">
                  {tools.map((tool) => (
                    <li key={tool.name} className="flex flex-wrap items-center gap-x-3 gap-y-0.5 px-4 py-1.5 hover:bg-raised">
                      {tool.writes ? (
                        <Chip tone="action">
                          <PenLine size={10} aria-hidden /> write
                        </Chip>
                      ) : (
                        <Chip tone="ok">
                          <Lock size={10} aria-hidden /> read
                        </Chip>
                      )}
                      <code className="min-w-0 border-0 bg-transparent p-0 font-mono text-[12.5px] break-all text-ink">{tool.name}</code>
                      <span className="w-full truncate pl-[60px] text-[12px] text-ink-3 md:ml-auto md:w-auto md:max-w-[55%] md:pl-0">{tool.title}</span>
                    </li>
                  ))}
                </ul>
                {!whole && (
                  <button
                    type="button"
                    onClick={() => setExpanded((s) => new Set(s).add(g.title))}
                    className="flex min-h-0 w-full cursor-pointer items-center justify-center gap-1.5 border-0 border-t border-hairline bg-transparent px-4 py-1.5 text-[12px] text-action hover:bg-raised"
                  >
                    <ChevronDown size={13} aria-hidden /> Show all {g.tools.length}
                  </button>
                )}
              </section>
            );
          })}
        </div>
      )}
      <p className="mt-3 mb-0 flex items-center gap-1.5 text-[12px] text-ink-3">
        <Sparkles size={12} aria-hidden /> Each call runs as you and is recorded in the audit log.
      </p>
    </div>
  );
}

function Flows({ setup }: { setup: McpSetup }) {
  if (!setup.flows.length) return <Empty>No worked flows are listed.</Empty>;
  return (
    <div className="grid grid-cols-1 gap-3 xl:grid-cols-2">
      {setup.flows.map((flow) => (
        <Card key={flow.title} title={flow.title}>
          {flow.summary && <p className="mt-0 text-[12.5px] text-ink-2">{flow.summary}</p>}
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
  );
}
