// SPDX-License-Identifier: MIT

/** The pieces Developer → API and Developer → MCP share, after rev-os's developer kit: the page
 * tabs (kept in `?tab=`, so a link can open one), code blocks with Copy on a dark plane, a
 * download button, and the keys: a form that calls `create_token`, the callout that shows a new
 * key once, and the table of the viewer's tokens (`list_sessions`) with Revoke (`revoke_session`).
 * A key is a lake API token: it acts as its owner and never outlives the sign-in that made it,
 * unless it is a standing key. The operator has no tokens; its key is the admin token. */
import { type ReactNode, useState } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { Check, Copy, Download, KeyRound, ShieldCheck, TriangleAlert } from "lucide-react";
import { act, useList, useRead } from "@/lib/api";
import { useSession, type Viewer } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { ConfirmButton } from "@/lib/overlay";
import { dateTime, relative } from "@/lib/format";
import type { UseQueryResult } from "@tanstack/react-query";
import { Badge, Button, Card, cx, DataTable, Field, Notice, Query, type Tone } from "@/lib/ui";
import { messageOf, NO_WORKSPACE } from "@/pages/account/signIn";
import type { SessionRow } from "@/pages/account/labels";
import {
  type Access,
  accessOf,
  type KeyForm,
  keyFormProblem,
  keyInput,
  type KeyScopes,
  keyState,
  LIFETIMES,
  type Lifetime,
  LIQUID_SCOPES,
  needsReason,
  type Purpose,
} from "./logic";

export { NO_WORKSPACE };

/** What Developer → MCP reads from `mcp_setup` (docs/api/web-app.md#mcp_setup). */
export type McpSetup = {
  origin: string;
  mcp_url: string;
  tool_groups: { title: string; summary: string; tools: { name: string; title: string; writes: boolean }[] }[];
  flows: { title: string; summary: string; steps: { tool: string; note: string }[] }[];
  scopes: KeyScopes;
};

/** `mcp_setup`, once for both pages (it is the same for every page of a sign-in). */
export function useMcpSetup() {
  return useRead<McpSetup>("mcp_setup", undefined, { ...NO_WORKSPACE, staleTime: 300_000 });
}

/** Lake's browser origin: the viewer's `external_origin`, else this page's. */
export function originOf(viewer: Pick<Viewer, "external_origin">): string {
  const origin = viewer.external_origin?.replace(/\/+$/, "");
  return origin || (typeof window === "undefined" ? "" : window.location.origin);
}

/* ------------------------------------------------------------------ tabs */

/** The page's tab, from `?tab=`; an unknown or missing one is the first. */
export function useTab<T extends string>(ids: readonly T[]): T {
  const [params] = useSearchParams();
  const asked = params.get("tab") as T | null;
  return asked && ids.includes(asked) ? asked : ids[0];
}

/** Underlined page tabs that are links (`?tab=<id>`), so the browser's back button and a shared
 * address open the same tab. */
export function PageTabs<T extends string>({ tabs, value }: { tabs: { id: T; label: ReactNode; count?: number }[]; value: T }) {
  return (
    <nav className="tabs mb-5" aria-label="Sections">
      {tabs.map((tab, index) => (
        <Link key={tab.id} to={index === 0 ? "?" : `?tab=${tab.id}`} replace aria-current={tab.id === value ? "page" : undefined}>
          {tab.label}
          {tab.count !== undefined && <span className="tab-count">{tab.count}</span>}
        </Link>
      ))}
    </nav>
  );
}

/* ------------------------------------------------------------------ small parts */

/** A small mono chip: an operation id, a protocol, a permission. */
export function Chip({ children, tone = "plain", title }: { children: ReactNode; tone?: "plain" | "ok" | "action" | "warn" | "bad"; title?: string }) {
  return (
    <span
      title={title}
      className={cx(
        "inline-flex items-center gap-1 rounded-[4px] border px-1.5 py-px font-mono text-[10.5px] leading-[1.6] whitespace-nowrap",
        tone === "plain" && "border-hairline bg-raised text-ink-2",
        tone === "ok" && "border-transparent bg-ok-soft text-ok",
        tone === "action" && "border-transparent bg-action-soft text-action",
        tone === "warn" && "border-transparent bg-warn-soft text-warn",
        tone === "bad" && "border-transparent bg-bad-soft text-bad",
      )}
    >
      {children}
    </span>
  );
}

/** A Copy button; says Copied for a moment. `copyId` marks it for the browser checks. */
export function CopyButton({ text, label = "Copy", className, copyId }: { text: string; label?: string; className?: string; copyId?: string }) {
  const [done, setDone] = useState(false);
  return (
    <button
      type="button"
      data-copy={copyId}
      onClick={() => {
        void navigator.clipboard?.writeText(text).then(
          () => {
            setDone(true);
            setTimeout(() => setDone(false), 1400);
          },
          () => undefined,
        );
      }}
      className={cx(
        "inline-flex min-h-0 cursor-pointer items-center gap-1 rounded-[4px] border px-2 py-0.5 font-mono text-[10.5px] transition-colors",
        done
          ? "border-transparent bg-ok-soft text-ok"
          : "border-[var(--orama-color-night-60)] bg-[var(--orama-color-night-80)] text-[var(--orama-color-night-20)] hover:text-[var(--orama-color-night-10)]",
        className,
      )}
    >
      {done ? <Check size={11} aria-hidden /> : <Copy size={11} aria-hidden />}
      {done ? "Copied" : label}
    </button>
  );
}

/** A copy-ready block of code on a dark terminal plane: a title bar with three dots, the title in
 * a mono kicker and Copy; an optional note under it. `id` names the code element (and its Copy
 * button's `data-copy`). */
export function CodeBlock({ title, code, note, id }: { title?: ReactNode; code: string; note?: ReactNode; id?: string }) {
  return (
    <div className="min-w-0 overflow-hidden rounded-[8px] border border-[var(--orama-color-night-60)] bg-[var(--orama-color-night-90)] shadow-[0_1px_2px_rgb(0_0_0/0.12)]">
      <div className="flex items-center gap-2 border-b border-[var(--orama-color-night-60)] bg-[var(--orama-color-night-80)] px-3.5 py-2">
        <span className="flex shrink-0 gap-1.5" aria-hidden>
          <span className="h-2.5 w-2.5 rounded-full bg-[var(--orama-color-night-60)]" />
          <span className="h-2.5 w-2.5 rounded-full bg-[var(--orama-color-night-60)]" />
          <span className="h-2.5 w-2.5 rounded-full bg-action opacity-80" />
        </span>
        <span className="ml-1 min-w-0 truncate font-mono text-[10.5px] tracking-[0.1em] text-[var(--orama-color-night-30)] uppercase">{title ?? "shell"}</span>
        <CopyButton text={code} className="ml-auto shrink-0" copyId={id} />
      </div>
      <pre className="m-0 overflow-x-auto border-0 bg-transparent px-4 py-3 font-mono text-[12.5px] leading-[1.65] text-[var(--orama-color-night-10)]">
        <code id={id} className="rounded-none border-0 bg-transparent p-0 text-current">
          {code}
        </code>
      </pre>
      {note && <div className="border-t border-[var(--orama-color-night-60)] px-3.5 py-2 text-[12px] leading-relaxed text-[var(--orama-color-night-30)]">{note}</div>}
    </div>
  );
}

/** The strip at the top of both Developer pages: what to call, and the chips that say how. */
export function EndpointStrip({ label, value, children }: { label: string; value: string; children?: ReactNode }) {
  return (
    <div className="mb-6 flex flex-wrap items-center gap-x-3 gap-y-2 rounded-[7px] border border-hairline bg-surface px-4 py-2.5 text-[12.5px]">
      <span className="label m-0">{label}</span>
      <code className="min-w-0 border-0 bg-transparent p-0 font-mono text-[13px] break-all text-ink">{value}</code>
      <CopyButton text={value} className="border-hairline bg-raised text-ink-2 hover:text-ink" />
      <span className="flex flex-wrap items-center gap-1.5 md:ml-auto">{children}</span>
    </div>
  );
}

/** A numbered step: the number, its title and a line of advice in a left rail, the content on
 * the right (rev-os's setup steps). One column on a phone. */
export function Step({ n, title, aside, children, id }: { n: number; title: string; aside?: ReactNode; children: ReactNode; id?: string }) {
  return (
    <section id={id} className="grid scroll-mt-20 grid-cols-1 gap-4 border-t border-hairline pt-6 pb-2 first:border-t-0 first:pt-0 lg:grid-cols-[230px_minmax(0,1fr)] lg:gap-8">
      <div>
        <div className="flex items-center gap-2.5">
          <span className="flex h-6 w-6 shrink-0 items-center justify-center rounded-full bg-action font-mono text-[11px] font-semibold text-on-action">{n}</span>
          <h3 className="m-0 text-[14.5px] font-semibold text-ink">{title}</h3>
        </div>
        {aside && <div className="mt-2 pl-[34px] text-[12.5px] leading-relaxed text-ink-3">{aside}</div>}
      </div>
      <div className="min-w-0 space-y-3">{children}</div>
    </section>
  );
}

/** A schema or a value as JSON, in a light scroll box. */
export function JsonBox({ value, max = "max-h-64" }: { value: string; max?: string }) {
  return <pre className={cx("m-0 overflow-auto rounded-[5px] border border-hairline bg-raised px-3 py-2 font-mono text-[11.5px] leading-[1.55] text-ink-2", max)}>{value}</pre>;
}

/** Fetch a same-origin document with the browser session and save it as `filename`. */
export async function downloadFrom(path: string, filename: string, transform?: (text: string) => string) {
  const response = await fetch(path, { credentials: "same-origin", headers: { Accept: "application/json, text/plain, */*" } });
  if (!response.ok) throw new Error(response.status === 404 ? "Not available on this Lake." : `The download failed (${response.status}).`);
  let text = await response.text();
  if (transform) text = transform(text);
  const url = URL.createObjectURL(new Blob([text], { type: response.headers.get("Content-Type") || "text/plain" }));
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

/** A button that downloads a document lake serves. */
export function DownloadButton({ path, filename, children, transform }: { path: string; filename: string; children: ReactNode; transform?: (text: string) => string }) {
  const [busy, setBusy] = useState(false);
  const toast = useToast();
  return (
    <Button
      loading={busy}
      onClick={async () => {
        setBusy(true);
        try {
          await downloadFrom(path, filename, transform);
        } catch (e) {
          toast(messageOf(e), "bad");
        } finally {
          setBusy(false);
        }
      }}
    >
      {!busy && <Download size={14} aria-hidden />}
      {children}
    </Button>
  );
}

/** Pretty-print a JSON document for saving; leave anything else as it came. */
export function prettyJson(text: string): string {
  try {
    return JSON.stringify(JSON.parse(text), null, 2);
  } catch {
    return text;
  }
}

/* ------------------------------------------------------------------ keys */

const ACCESS: Record<Purpose, { value: Access; label: string; help: string }[]> = {
  api: [
    { value: "all", label: "Every operation", help: "Everything you may do. It can never do more than you." },
    { value: "read", label: "Read-only", help: "Only the read operations; every write is refused." },
    { value: "named", label: "Named capabilities", help: "Only the capabilities you list, by name." },
  ],
  mcp: [
    { value: "all", label: "Every tool", help: "Every tool you may use. It can never do more than you." },
    { value: "read", label: "Read-only tools", help: "Only the read tools; every write is refused." },
    { value: "named", label: "Named tools", help: "Only the tools you list, by name." },
  ],
};

const EMPTY_FORM: Omit<KeyForm, "purpose"> = { name: "", access: "all", named: "", lifetime: "", seconds: "", isStatic: false, reason: "" };

/** The key form: name, access, lifetime and, for a standing or static key, its reason. It calls
 * `create_token` with the input `keyInput` builds; the secret comes back once, to `onCreated`. */
export function KeyCreator({ purpose, scopes, onCreated }: { purpose: Purpose; scopes: KeyScopes; onCreated: (secret: string) => void }) {
  const [form, setForm] = useState<KeyForm>({ ...EMPTY_FORM, purpose });
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const set = <K extends keyof KeyForm>(key: K, value: KeyForm[K]) => setForm((f) => ({ ...f, [key]: value }));
  const problem = keyFormProblem(form);
  const access = ACCESS[purpose];

  const create = async () => {
    if (problem) {
      setError(problem);
      return;
    }
    setBusy(true);
    setError(null);
    try {
      const value = await act<{ secret?: string }>("create_token", keyInput(form, scopes), NO_WORKSPACE);
      if (!value.secret) throw new Error("The key was made but its secret did not come back. Revoke it below and make another.");
      onCreated(value.secret);
      setForm({ ...EMPTY_FORM, purpose });
    } catch (e) {
      setError(messageOf(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <form
      className="space-y-3"
      onSubmit={(e) => {
        e.preventDefault();
        void create();
      }}
    >
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-[minmax(0,1.3fr)_minmax(0,1fr)]">
        <Field label="Name" help="Where it will be used: you will see it in the list below.">
          <input name="name" value={form.name} onChange={(e) => set("name", e.target.value)} placeholder={purpose === "mcp" ? "Claude Code on my laptop" : "Nightly export"} />
        </Field>
        <Field label="Lifetime" help="A key never outlives the sign-in that made it, unless it is a standing key your space allows.">
          <select name="expires_in_seconds" value={form.lifetime} onChange={(e) => set("lifetime", e.target.value as Lifetime)}>
            {LIFETIMES.map((option) => (
              <option key={option.value} value={option.value}>
                {option.label}
              </option>
            ))}
          </select>
        </Field>
      </div>
      <fieldset className="m-0 border-0 p-0">
        <legend className="label mb-1.5">Access</legend>
        <div className="grid grid-cols-1 gap-2 sm:grid-cols-3">
          {access.map((option) => (
            <label
              key={option.value}
              className={cx(
                "flex cursor-pointer gap-2 rounded-[6px] border px-3 py-2 text-[13px]",
                form.access === option.value ? "border-action bg-action-soft" : "border-hairline bg-surface hover:border-rule",
              )}
            >
              <input type="radio" name="access" value={option.value} checked={form.access === option.value} onChange={() => set("access", option.value)} className="mt-[3px]" />
              <span>
                <span className="block font-semibold text-ink">{option.label}</span>
                <span className="block text-[12px] leading-snug text-ink-2">{option.help}</span>
              </span>
            </label>
          ))}
        </div>
      </fieldset>
      {form.access === "named" && (
        <Field
          label="Capabilities, comma separated"
          help={
            <>
              Capability names, or <code>registry_pull</code> to pull images. For liquid push and pull:{" "}
              <button type="button" className="min-h-0 cursor-pointer border-0 bg-transparent p-0 text-action underline" onClick={() => set("named", LIQUID_SCOPES)}>
                <code>{LIQUID_SCOPES}</code>
              </button>
              ; the key keeps your repository permissions.
            </>
          }
        >
          <input name="scopes" value={form.named} onChange={(e) => set("named", e.target.value)} placeholder="list_repositories, get_repository" />
        </Field>
      )}
      {form.lifetime === "custom" && (
        <Field label="Lifetime in seconds">
          <input type="number" name="seconds" min={1} value={form.seconds} onChange={(e) => set("seconds", e.target.value)} placeholder="3600" />
        </Field>
      )}
      <label className="flex items-center gap-2 text-[13px] text-ink-2">
        <input type="checkbox" name="static" checked={form.isStatic} onChange={(e) => set("isStatic", e.target.checked)} /> Static exception: a key that is not tied
        to a sign-in session (needs a reason)
      </label>
      {needsReason(form) && (
        <Field label="Why this key exists" help="Kept with the key. A standing key also needs your space's opt-in.">
          <input name="reason" value={form.reason} onChange={(e) => set("reason", e.target.value)} placeholder="the nightly sync from our data warehouse" />
        </Field>
      )}
      {form.lifetime === "never" && form.access === "all" && (
        <p className="m-0 text-[12px] text-ink-3">A key that never expires names what it may call, so this one names every capability Lake has today.</p>
      )}
      {error && <Notice tone="danger">{error}</Notice>}
      <div className="flex flex-wrap items-center gap-3">
        <Button type="submit" variant="primary" loading={busy}>
          {!busy && <KeyRound size={14} aria-hidden />}
          {purpose === "mcp" ? "Create an MCP key" : "Create an API key"}
        </Button>
        <span className="text-[12px] text-ink-3">The key is shown once. Lake keeps only its hash.</span>
      </div>
    </form>
  );
}

/** The one moment a new key is visible. */
export function NewKeyCallout({ value, onDone, purpose }: { value: string; onDone: () => void; purpose: Purpose }) {
  return (
    <div className="rounded-[7px] border border-ok bg-ok-soft px-4 py-3.5" role="status">
      <div className="flex items-center gap-2 text-[13.5px] font-semibold text-ok">
        <ShieldCheck size={15} aria-hidden /> Your New Key: Copy It Now
      </div>
      <p className="mt-0.5 mb-0 text-[12.5px] text-ink-2">
        This is the only time Lake shows it. Keep it in a password manager or your client's secret settings.{" "}
        {purpose === "mcp" ? "It is filled into every setup block below until you leave this page." : "It is filled into Quick start and every operation's curl until you leave this page."}
      </p>
      <div className="mt-2.5 flex items-center gap-2 rounded-[6px] border border-hairline bg-surface px-3 py-2">
        <code id={purpose === "mcp" ? "mcp-key" : "api-key"} className="min-w-0 flex-1 truncate font-mono text-[12.5px] text-ink">
          {value}
        </code>
        <CopyButton text={value} className="border-hairline bg-raised text-ink-2 hover:text-ink" />
      </div>
      <div className="mt-2.5">
        <Button onClick={onDone}>I've stored it</Button>
      </div>
    </div>
  );
}

const STATE_TONE: Record<ReturnType<typeof keyState>, Tone> = { active: "success", revoked: "danger", expired: "warning" };
const STATE_LABEL: Record<ReturnType<typeof keyState>, string> = { active: "Active", revoked: "Revoked", expired: "Expired" };

/** The viewer's API tokens (`list_sessions`, kinds `api_token` and `static`) with Revoke. */
export function KeysTable({ scopes }: { scopes?: KeyScopes }) {
  const toast = useToast();
  const sessions = useList<SessionRow & { last_used_at?: string | null }>("list_sessions", undefined, { ...NO_WORKSPACE, pick: "items" });
  const [showRevoked, setShowRevoked] = useState(false);
  const tokens = sessions.data?.filter((row) => (row.kind === "api_token" || row.kind === "static") && (showRevoked || keyState(row) === "active"));
  const revoke = async (row: SessionRow) => {
    await act("revoke_session", { id: row.id }, NO_WORKSPACE);
    toast("Key revoked: it stops working now");
  };

  return (
    <Card
      title="Your Keys"
      actions={
        <label className="flex items-center gap-2 text-[12px] text-ink-3">
          <input type="checkbox" checked={showRevoked} onChange={(e) => setShowRevoked(e.target.checked)} /> Show revoked and expired
        </label>
      }
    >
      <DataTable
        q={sessions}
        what="your keys"
        rows={tokens}
        rowKey={(row) => row.id}
        empty="No keys yet. Create one above to call Lake from a program or an agent."
        initialSort={{ key: "created", dir: "descending" }}
        columns={[
          {
            key: "name",
            label: "Key",
            sort: (row) => row.name?.toLowerCase(),
            render: (row) => (
              <div>
                <div className="font-medium text-ink">{row.name || "—"}</div>
                <div className="font-mono text-[11px] text-ink-3">{row.kind === "static" ? "static" : "api token"}</div>
              </div>
            ),
          },
          {
            key: "access",
            label: "Access",
            render: (row) => {
              const access = accessOf(row.scopes, scopes);
              return (
                <Chip tone={access.kind === "read" ? "ok" : access.kind === "all" ? "action" : "plain"} title={access.title}>
                  {access.label}
                </Chip>
              );
            },
          },
          {
            key: "expires",
            label: "Expires",
            sort: (row) => row.expires_at ?? "9999",
            render: (row) =>
              row.expires_at ? (
                <span className="tabular-nums" title={dateTime(row.expires_at)}>
                  {relative(row.expires_at)}
                </span>
              ) : (
                <span className="inline-flex items-center gap-1 text-warn" title={row.reason || undefined}>
                  <TriangleAlert size={12} aria-hidden /> Never
                </span>
              ),
          },
          {
            key: "last_used",
            label: "Last used",
            sort: (row) => row.last_used_at ?? "",
            render: (row) => <span className="tabular-nums text-ink-2">{relative(row.last_used_at, Date.now(), "never")}</span>,
          },
          { key: "created", label: "Created", sort: (row) => row.created_at ?? "", render: (row) => <span className="tabular-nums text-ink-2">{dateTime(row.created_at)}</span> },
          {
            key: "state",
            label: "State",
            render: (row) => {
              const state = keyState(row);
              return <Badge tone={STATE_TONE[state]}>{STATE_LABEL[state]}</Badge>;
            },
          },
          {
            key: "revoke",
            label: <span className="sr-only">Actions</span>,
            className: "text-right",
            render: (row) =>
              keyState(row) === "revoked" ? null : (
                <ConfirmButton
                  title="Revoke the key?"
                  confirmLabel="Revoke"
                  confirm={
                    <>
                      <b>{row.name || "This key"}</b> stops working at once, and so does anything that uses it.
                    </>
                  }
                  onConfirm={() => revoke(row)}
                >
                  Revoke
                </ConfirmButton>
              ),
          },
        ]}
      />
    </Card>
  );
}

/** The operator signs in with the admin token and has no tokens of its own: say so. */
export function OperatorKeys() {
  return (
    <Notice title="Your Key Is the Admin Token">
      You are signed in as the operator. The operator's key is the instance's admin token (<code>LAKE_ADMIN_TOKEN</code>), not a token made here: send it as{" "}
      <code>Authorization: Bearer &lt;admin token&gt;</code>. Keys made on this page belong to a person and act as that person; sign in as one to make them.
    </Notice>
  );
}

/** The Keys tab of both pages: the form, or the new key once, then the table. */
export function KeysPanel({
  purpose,
  setup,
  fresh,
  onCreated,
  onDone,
}: {
  purpose: Purpose;
  /** `mcp_setup`, for the read-only scopes. */
  setup: UseQueryResult<McpSetup>;
  fresh: string | null;
  onCreated: (secret: string) => void;
  onDone: () => void;
}) {
  const { viewer } = useSession();
  if (viewer.administrator) return <OperatorKeys />;
  const scopes = setup.data?.scopes;
  return (
    <div className="space-y-4">
      <Card title={purpose === "mcp" ? "Create an MCP Key" : "Create an API Key"}>
        {fresh ? (
          <NewKeyCallout value={fresh} onDone={onDone} purpose={purpose} />
        ) : (
          <Query q={setup} what="the key options" lines={4}>
            {(value) => <KeyCreator purpose={purpose} scopes={value.scopes} onCreated={onCreated} />}
          </Query>
        )}
      </Card>
      <KeysTable scopes={scopes} />
      <p className="text-[12px] text-ink-3">
        A key works for both the REST API and MCP. Sign-in sessions and devices are on <Link to="/sessions">Sessions</Link>; every token is also on{" "}
        <Link to="/tokens">Tokens</Link>.
      </p>
    </div>
  );
}
