// SPDX-License-Identifier: MIT

/** Developer → API (`/developer/api`): everything needed to call lake from a program, after
 * rev-os's Developer → API. Keys makes and revokes the viewer's API tokens (`create_token`,
 * `list_sessions`, `revoke_session`); Quick start shows the calls with the viewer's origin and
 * key; Operations is `/v1/openapi.json` itself, every operation grouped as `mcp_setup` groups its
 * tool, searchable, each with its parameters, request and response schemas and a curl line. The
 * document is generated from the running registry, never written by hand. */
import { useMemo, useState } from "react";
import { useQuery } from "@tanstack/react-query";
import { BookOpen, ChevronDown, ChevronRight, Lock, PenLine, Plug, Search, ShieldCheck } from "lucide-react";
import { ApiError, type Row } from "@/lib/api";
import { plural } from "@/lib/format";
import { useSession } from "@/lib/session";
import { Button, Card, cx, Empty, ErrorBlock, FilterChips, Notice, Page, Skeleton } from "@/lib/ui";
import { CodeBlock, Chip, DownloadButton, EndpointStrip, JsonBox, KeysPanel, originOf, PageTabs, prettyJson, useMcpSetup, useTab } from "./parts";
import { curlFor, groupOperations, type Operation, operationsOf, quickStartFetch, quickStartPython, requestBodyOf, responseSchemaOf, schemaText } from "./logic";

const TABS = ["keys", "start", "operations"] as const;
type Tab = (typeof TABS)[number];

/** The OpenAPI document, as the browser's session sees it (an agent task sees `workspace_id` as
 * optional; everyone else the declared schema). */
function useOpenApi() {
  return useQuery<Row>({
    queryKey: ["openapi.json"],
    staleTime: 300_000,
    queryFn: async ({ signal }) => {
      let response: Response;
      try {
        response = await fetch("/v1/openapi.json", { headers: { Accept: "application/json" }, credentials: "same-origin", signal });
      } catch (error) {
        if ((error as Error).name === "AbortError") throw error;
        throw new ApiError(0, "network", "Lake could not be reached. Check your connection and retry.");
      }
      if (!response.ok) throw new ApiError(response.status, "error", `The OpenAPI document did not load (${response.status}).`);
      return (await response.json()) as Row;
    },
  });
}

export default function ApiPage() {
  const tab = useTab(TABS);
  const { viewer } = useSession();
  const [fresh, setFresh] = useState<string | null>(null);
  const setup = useMcpSetup();
  const who = viewer.administrator ? "the operator, with the admin token" : "you, with exactly your permissions";

  return (
    <Page
      eyebrow="Developer"
      title="API"
      sub={`Call Lake from a program as ${who}. Every page on this site, Ask Orama and the MCP server use this same API: one registry, REST under /v1, OpenAPI and MCP.`}
      actions={
        <>
          <DownloadButton path="/v1/openapi.json" filename="lake-openapi.json" transform={prettyJson}>
            openapi.json
          </DownloadButton>
          <Button to="/docs/developer">
            <BookOpen size={14} aria-hidden /> API guide
          </Button>
          <Button to="/developer/mcp" variant="primary">
            <Plug size={14} aria-hidden /> Set up MCP
          </Button>
        </>
      }
    >
      <EndpointStrip label="Base URL" value={`${originOf(viewer)}/v1`}>
        <Chip>REST · JSON</Chip>
        <Chip>OpenAPI 3.1</Chip>
        <Chip>{"{ok, value, duration_ms}"}</Chip>
        <Chip tone="action">Authorization: Bearer &lt;key&gt;</Chip>
      </EndpointStrip>
      <PageTabs<Tab>
        value={tab}
        tabs={[
          { id: "keys", label: "Keys" },
          { id: "start", label: "Quick start" },
          { id: "operations", label: "Operations" },
        ]}
      />
      {tab === "keys" && (
        <div className="space-y-4">
          <KeysPanel purpose="api" setup={setup} fresh={fresh} onCreated={setFresh} onDone={() => setFresh(null)} />
          <Rules />
        </div>
      )}
      {tab === "start" && <QuickStart keyValue={fresh} />}
      {tab === "operations" && <Operations keyValue={fresh} groups={setup.data?.tool_groups} />}
    </Page>
  );
}

function Rules() {
  const rules: [string, string][] = [
    ["Your permissions, nothing more", "A key acts as you and can never do more than the credential that made it. A 403 means you may not; the pages would refuse it too."],
    ["Read-only keys", "A read-only key names every read tool and is refused every write. Give a program that only reports a read-only key."],
    ["Expiry and revocation", "A key expires no later than the sign-in that made it would have, at most 30 days, unless it is a standing key. Signing out does not end it. Revoking it, or Sign out everywhere, stops it at once."],
    ["Every call is audited", "Each call is recorded under your name in the audit log, as if you had made it on the site."],
    ["One envelope", "Every answer is {ok, value, duration_ms} or {ok: false, error: {code, message, path}}; path points at the input field at fault."],
    ["No secrets come back", "A key's secret is shown once, when it is made. No read returns a credential, a token or another person's key."],
  ];
  return (
    <Card title="The Rules a Key Lives By">
      <div className="grid grid-cols-1 gap-4 md:grid-cols-3">
        {rules.map(([title, text]) => (
          <div key={title} className="flex gap-2.5">
            <ShieldCheck size={15} className="mt-[3px] shrink-0 text-action" aria-hidden />
            <div>
              <div className="text-[13px] font-semibold text-ink">{title}</div>
              <div className="mt-0.5 text-[12.5px] leading-relaxed text-ink-2">{text}</div>
            </div>
          </div>
        ))}
      </div>
    </Card>
  );
}

function QuickStart({ keyValue }: { keyValue: string | null }) {
  const { viewer, workspaceId, space } = useSession();
  const base = originOf(viewer);
  const key = keyValue ?? "$LAKE_KEY";
  const customer = space?.id ?? viewer.customer_id ?? "<customer id>";
  const workspace = workspaceId ?? "<workspace id>";
  const fetchCode = quickStartFetch(base, keyValue);
  const pythonCode = quickStartPython(base, keyValue);
  return (
    <div className="space-y-4">
      {!keyValue && (
        <Notice title="Use Your Own Key">
          The examples read the key from the <code>LAKE_KEY</code> environment variable: <code>$LAKE_KEY</code> in the shell,{" "}
          <code>process.env.LAKE_KEY</code> in JavaScript and <code>os.environ["LAKE_KEY"]</code> in Python. Make a key on the Keys tab and set it with{" "}
          <code>export LAKE_KEY=…</code>, or it is filled in here until you leave the page.
        </Notice>
      )}
      <div className="grid grid-cols-1 gap-4 lg:grid-cols-2">
        <CodeBlock title="who am I" code={`curl -s ${base}/v1/viewer \\\n  -H "Authorization: Bearer ${key}"`} note="viewer needs no key; with one it names you, your spaces and their workspaces." />
        <CodeBlock title="a list read: GET, input in the query" code={`curl -s "${base}/v1/list_repositories?limit=5" \\\n  -H "Authorization: Bearer ${key}"`} />
        <CodeBlock
          title="an action: POST, input as JSON"
          code={`curl -s -X POST ${base}/v1/create_repository \\\n  -H "Authorization: Bearer ${key}" \\\n  -H "Content-Type: application/json" \\\n  -d '${JSON.stringify({ customer_id: customer, workspace_id: workspace, name: "my-first-repo" })}'`}
          note="An action changes something and is recorded in the audit log under your name. A read-only key is refused it."
        />
        <CodeBlock title="from JavaScript" code={fetchCode} />
        <CodeBlock
          title="from Python: send a User-Agent"
          code={pythonCode}
          note={'Every call needs a User-Agent header. Python\'s urllib default is refused at the edge with 403 "error code: 1010"; curl, requests and fetch send one already.'}
        />
        <CodeBlock
          title="in a workspace: /w/<id>/v1"
          code={`curl -s "${base}/w/${workspace}/v1/workspace_status?customer_id=${customer}&workspace_id=${workspace}" \\\n  -H "Authorization: Bearer ${key}"`}
          note="Under /w/<workspace id>, Lake fills workspace_id from the address and refuses a different one; the pages call it this way."
        />
        <CodeBlock title="the whole contract" code={`curl -s ${base}/v1/openapi.json > lake-openapi.json`} note="OpenAPI 3.1. Load it into Postman, Insomnia or any OpenAPI tool." />
      </div>
      <div className="grid grid-cols-1 gap-4 md:grid-cols-3">
        <Card title="The Envelope">
          <p className="m-0 text-[12.5px] leading-relaxed text-ink-2">
            Success is <code>{`{"ok": true, "value": …, "duration_ms": …}`}</code>; the capability's answer is <code>value</code>. A failure is{" "}
            <code>{`{"ok": false, "error": {"code", "message", "path"}}`}</code> with the matching status: 400 <code>invalid_input</code>, 401{" "}
            <code>unauthenticated</code>, 403 <code>unauthorized</code>, 404 <code>not_found</code>, 409 <code>conflict</code>.
          </p>
        </Card>
        <Card title="Reads and Actions">
          <p className="m-0 text-[12.5px] leading-relaxed text-ink-2">
            A read is <code>GET /v1/&lt;name&gt;?&lt;input&gt;</code>; an action is <code>POST /v1/&lt;name&gt;</code> with the input as a JSON body. Lists answer{" "}
            <code>{`{items: […]}`}</code> or a named list; most take <code>limit</code>.
          </p>
        </Card>
        <Card title="Workspaces">
          <p className="m-0 text-[12.5px] leading-relaxed text-ink-2">
            Every Design, Train and Ship call names your space (<code>customer_id</code>) and workspace (<code>workspace_id</code>); Lake never guesses. A
            workspace's own capabilities are reached through <code>workspace_query</code> and <code>workspace_act</code>.
          </p>
        </Card>
      </div>
    </div>
  );
}

const METHOD_TONE: Record<string, string> = {
  GET: "bg-ok-soft text-ok",
  POST: "bg-action-soft text-action",
  PUT: "bg-warn-soft text-warn",
  PATCH: "bg-warn-soft text-warn",
  DELETE: "bg-bad-soft text-bad",
};

function Operations({ keyValue, groups }: { keyValue: string | null; groups?: { title: string; tools: { name: string }[] }[] }) {
  const doc = useOpenApi();
  const [needle, setNeedle] = useState("");
  const [access, setAccess] = useState<"read" | "write" | null>(null);
  const [open, setOpen] = useState<string | null>(null);
  const all = useMemo(() => operationsOf(doc.data, groups), [doc.data, groups]);
  const order = useMemo(() => (groups ?? []).map((g) => g.title), [groups]);
  const grouped = useMemo(() => groupOperations(access ? all.filter((o) => (access === "write") === o.writes) : all, needle, order), [all, access, needle, order]);
  const writes = all.filter((o) => o.writes).length;

  if (doc.isLoading) return <Skeleton lines={10} />;
  if (doc.isError) return <ErrorBlock error={doc.error} retry={() => void doc.refetch()} what="the OpenAPI document" />;
  const shown = grouped.reduce((n, [, ops]) => n + ops.length, 0);
  const components = (doc.data?.components?.schemas ?? {}) as Record<string, Row>;

  return (
    <div>
      <div className="mb-3 flex flex-wrap items-center gap-3">
        <label className="relative flex w-full max-w-sm items-center">
          <Search size={14} className="pointer-events-none absolute left-2.5 text-ink-3" aria-hidden />
          <span className="sr-only">Find an operation</span>
          <input type="search" value={needle} onChange={(e) => setNeedle(e.target.value)} placeholder="Find an operation: path, name or group" className="w-full pl-8" />
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
        <span className="font-mono text-[12px] text-ink-3 tabular-nums sm:ml-auto">
          {needle || access ? `${shown} of ${all.length}` : all.length} operations · {plural(grouped.length, "group")} · OpenAPI {String(doc.data?.openapi ?? "3.1")}
        </span>
      </div>
      {!grouped.length ? (
        <Empty>{all.length ? (needle ? `No operation matches “${needle}”.` : "No operation matches.") : "The OpenAPI document lists no operations."}</Empty>
      ) : (
        <div className="space-y-3">
          {grouped.map(([group, ops]) => (
            <section key={group} className="overflow-hidden rounded-[6px] border border-hairline bg-surface">
              <div className="flex items-center gap-2 border-b border-hairline px-4 py-2">
                <span className="label m-0">{group}</span>
                <span className="font-mono text-[11px] text-ink-3">{ops.length}</span>
              </div>
              <ul className="m-0 list-none divide-y divide-hairline p-0">
                {ops.map((o) => {
                  const id = `${o.method} ${o.path}`;
                  const isOpen = open === id;
                  return (
                    <li key={id}>
                      <button
                        type="button"
                        aria-expanded={isOpen}
                        onClick={() => setOpen(isOpen ? null : id)}
                        className="flex min-h-0 w-full cursor-pointer items-center justify-start gap-3 border-0 bg-transparent px-4 py-2 text-left hover:bg-raised"
                      >
                        {isOpen ? <ChevronDown size={13} className="shrink-0 text-ink-3" aria-hidden /> : <ChevronRight size={13} className="shrink-0 text-ink-3" aria-hidden />}
                        <span className={cx("w-[52px] shrink-0 rounded-[3px] py-px text-center font-mono text-[10.5px] font-semibold", METHOD_TONE[o.method] ?? "bg-raised text-ink-2")}>{o.method}</span>
                        <code className="min-w-0 truncate border-0 bg-transparent p-0 font-mono text-[12.5px] text-ink">{o.path}</code>
                        <span className="ml-auto hidden max-w-[45%] truncate text-[12px] text-ink-3 md:inline">{o.summary}</span>
                      </button>
                      {isOpen && <OperationDetail o={o} components={components} keyValue={keyValue} />}
                    </li>
                  );
                })}
              </ul>
            </section>
          ))}
        </div>
      )}
    </div>
  );
}

function OperationDetail({ o, components, keyValue }: { o: Operation; components: Record<string, Row>; keyValue: string | null }) {
  const { viewer } = useSession();
  const params = ((o.op.parameters ?? []) as Row[]).filter((p) => p.in === "query" || p.in === "path");
  const body = o.method === "GET" ? null : requestBodyOf(o.op, components);
  const response = responseSchemaOf(o.op, components);
  const curl = curlFor(o, originOf(viewer), keyValue ?? "$LAKE_KEY", components);
  return (
    <div className="space-y-3 border-t border-hairline bg-raised px-4 py-3">
      {o.description && <p className="m-0 text-[12.5px] whitespace-pre-line text-ink-2">{o.description.trim()}</p>}
      <div className="flex flex-wrap gap-1.5">
        {o.operationId && <Chip>{o.operationId}</Chip>}
        {o.writes ? (
          <Chip tone="action">
            <PenLine size={10} aria-hidden /> write: refused to a read-only key
          </Chip>
        ) : (
          <Chip tone="ok">
            <Lock size={10} aria-hidden /> read
          </Chip>
        )}
        {o.needs && <Chip title="the permission the call needs on its object">needs {o.needs}</Chip>}
        {o.public && <Chip tone="warn">no key needed</Chip>}
      </div>
      {params.length > 0 && (
        <div>
          <div className="label mb-1">Parameters</div>
          <div className="overflow-hidden rounded-[5px] border border-hairline bg-surface">
            {params.map((p) => (
              <div key={`${p.in}:${p.name}`} className="flex flex-wrap items-baseline gap-x-3 gap-y-0.5 border-b border-hairline px-3 py-1.5 text-[12.5px] last:border-b-0">
                <code className="font-mono text-ink">{p.name}</code>
                <span className="font-mono text-[10.5px] text-ink-3">{p.in}</span>
                <span className="font-mono text-[10.5px] text-ink-3">{schemaType(p.schema)}</span>
                {p.required && <span className="font-mono text-[10px] text-bad">required</span>}
                <span className="min-w-0 text-ink-2 md:ml-auto md:max-w-[60%] md:truncate">{p.description ?? ""}</span>
              </div>
            ))}
          </div>
        </div>
      )}
      {body && (
        <div>
          <div className="label mb-1">
            Request body <span className="tracking-normal normal-case">· {body.type}</span>
          </div>
          <JsonBox value={schemaText(body.schema)} />
        </div>
      )}
      {response && (
        <div>
          <div className="label mb-1">
            Response <span className="tracking-normal normal-case">· {response.enveloped ? "the value inside {ok, value, duration_ms}" : response.type}</span>
          </div>
          <JsonBox value={schemaText(response.schema)} />
        </div>
      )}
      <CodeBlock title="copy as curl" code={curl} />
    </div>
  );
}

function schemaType(schema: Row | undefined): string {
  if (!schema) return "";
  const type = Array.isArray(schema.type) ? schema.type.join(" | ") : schema.type;
  if (type === "array" && schema.items?.type) return `${schema.items.type}[]`;
  return String(type ?? "");
}
