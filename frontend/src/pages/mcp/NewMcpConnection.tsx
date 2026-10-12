/** New MCP connection (lake#652, docs/ui/mcp-connections.md): a `Modal sheet`. Where the server runs
 * comes first. A **remote** server: its URL, a name (its prefix shown as the person types), the
 * transport, and how Lake authenticates: none, a bearer token kept in a secret of this workspace (a
 * mount, the secret's name and its field), or an OAuth sign-in a person does on the connection's
 * page. "Test" connects without keeping anything. A server that **runs here** (lake#659): an npm or
 * Python package Lake runs as an App of this workspace, its arguments and its environment, each
 * variable a plain value or a field of a secret in the chosen mount, where Lake also keeps the
 * runner's token. "Add connection" keeps it. No token or secret value is ever typed here. */
import { useId, useState } from "react";
import { act, useList, type Row } from "@/lib/api";
import { Banner, Button, Field, FormError, fieldError } from "@/lib/ui";
import { Modal, SheetActions } from "@/lib/overlay";
import { useWorkspace } from "@/pages/train/workspace";
import { argsOf, envNameProblem, prefixOf, type McpConnection } from "./logic";

type AuthKind = "none" | "bearer" | "oauth";
type Where = "remote" | "stdio";
type EnvRow = { name: string; source: "value" | "secret"; value: string; secret: string; field: string };
type Tested = { ok: boolean; status?: string; error?: string; server?: { name?: string; version?: string }; transport?: string; tools?: string[]; milliseconds?: number };

export function NewMcpConnection({ onClose, onCreated }: { onClose: () => void; onCreated: (c: McpConnection) => void }) {
  const { customerId, workspaceId } = useWorkspace();
  const formId = useId();
  const [where, setWhere] = useState<Where>("remote");
  const [runtime, setRuntime] = useState<"node" | "python">("node");
  const [pkg, setPkg] = useState("");
  const [command, setCommand] = useState("");
  const [argsText, setArgsText] = useState("");
  const [env, setEnv] = useState<EnvRow[]>([]);
  const [name, setName] = useState("");
  const [url, setUrl] = useState("");
  const [transport, setTransport] = useState("auto");
  const [kind, setKind] = useState<AuthKind>("none");
  const [mountId, setMountId] = useState("");
  const [secretName, setSecretName] = useState("");
  const [field, setField] = useState("token");
  const [busy, setBusy] = useState(false);
  const [testing, setTesting] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const [tested, setTested] = useState<Tested | null>(null);
  const mounts = useList<Row>(kind === "bearer" || where === "stdio" ? "list_secret_mounts" : null, { customer_id: customerId, workspace_id: workspaceId, limit: 200 }, { pick: "items" });
  const prefix = prefixOf(name);

  const auth = () => (kind === "bearer" ? { kind, secret: { mount_id: mountId, name: secretName.trim(), field: field.trim() } } : { kind });

  const test = async () => {
    setTesting(true);
    setError(null);
    setTested(null);
    try {
      setTested(await act<Tested>("test_mcp_connection", { customer_id: customerId, workspace_id: workspaceId, url: url.trim(), transport, auth: auth() }));
    } catch (e) {
      setError(e);
    } finally {
      setTesting(false);
    }
  };

  const stdio = () => ({
    runtime,
    package: pkg.trim(),
    ...(command.trim() ? { command: command.trim() } : {}),
    args: argsOf(argsText),
    mount_id: mountId,
    env: Object.fromEntries(env.filter((row) => row.name.trim()).map((row) => [row.name.trim(), row.source === "value" ? row.value : { secret: { mount_id: mountId, name: row.secret.trim(), field: row.field.trim() } }])),
  });

  const create = async () => {
    setBusy(true);
    setError(null);
    try {
      const input = where === "stdio" ? { stdio: stdio() } : { url: url.trim(), transport, auth: auth() };
      onCreated(await act<McpConnection>("create_mcp_connection", { customer_id: customerId, workspace_id: workspaceId, name: name.trim(), ...input }));
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };

  const envProblem = env.some((row) => !!row.name.trim() && (!!envNameProblem(row.name.trim()) || (row.source === "secret" && (!row.secret.trim() || !row.field.trim()))));
  const ready =
    !!name.trim() &&
    (where === "stdio" ? !!pkg.trim() && !!mountId && !envProblem : !!url.trim() && (kind !== "bearer" || (!!mountId && !!secretName.trim() && !!field.trim())));
  const actions = (
    <>
      <Button onClick={onClose}>Cancel</Button>
      {where === "remote" && (
        <Button onClick={() => void test()} loading={testing} disabled={!url.trim() || kind === "oauth" || (kind === "bearer" && !ready)}>
          Test
        </Button>
      )}
      <Button variant="primary" type="submit" form={formId} loading={busy} disabled={!ready}>
        Add connection
      </Button>
    </>
  );

  return (
    <Modal title="New MCP Connection" subtitle="An MCP server this workspace will use." onClose={onClose} sheet width={600}>
      <form
        id={formId}
        className="stack"
        onSubmit={(e) => {
          e.preventDefault();
          if (ready) void create();
        }}
      >
        <FormError error={error && !fieldError(error, "url") && !fieldError(error, "name") && !fieldError(error, "auth") ? error : null} />
        <fieldset className="stack m-0 border-0 p-0">
          <legend className="field-label">Where the server runs</legend>
          <div className="flex flex-wrap gap-3" role="radiogroup" aria-label="Where the server runs">
            {(["remote", "stdio"] as Where[]).map((w) => (
              <label key={w} className="inline-flex items-center gap-1.5 text-[13.5px]">
                <input type="radio" name="where" checked={where === w} onChange={() => setWhere(w)} />
                {w === "remote" ? "At a URL" : "Here, from a package (stdio)"}
              </label>
            ))}
          </div>
        </fieldset>
        {where === "stdio" ? (
          <StdioFields
            runtime={runtime}
            setRuntime={setRuntime}
            pkg={pkg}
            setPkg={setPkg}
            command={command}
            setCommand={setCommand}
            argsText={argsText}
            setArgsText={setArgsText}
            env={env}
            setEnv={setEnv}
            mountId={mountId}
            setMountId={setMountId}
            mounts={mounts.data ?? []}
            mountsLoading={mounts.isLoading}
            error={error}
          />
        ) : (
        <Field label="Server URL" required help="Streamable HTTP or HTTP+SSE, over https. Lake connects only to public addresses." error={fieldError(error, "url")}>
          <input value={url} onChange={(e) => setUrl(e.target.value)} placeholder="https://mcp.example.com/mcp" inputMode="url" autoComplete="off" spellCheck={false} />
        </Field>
        )}
        <Field
          label="Name"
          required
          help={prefix ? <>Its tools are named <code>{prefix}__&lt;tool&gt;</code> outside the connection.</> : "How people and agents will know it."}
          error={fieldError(error, "name")}
        >
          <input value={name} onChange={(e) => setName(e.target.value)} placeholder="Ticket Desk" autoComplete="off" />
        </Field>
        {where === "remote" && (
        <Field label="Transport" help="Automatic tries Streamable HTTP, then the older HTTP+SSE.">
          <select value={transport} onChange={(e) => setTransport(e.target.value)}>
            <option value="auto">Automatic</option>
            <option value="streamable_http">Streamable HTTP</option>
            <option value="sse">HTTP+SSE (older servers)</option>
          </select>
        </Field>
        )}
        {where === "remote" && (
        <fieldset className="stack m-0 border-0 p-0">
          <legend className="field-label">Authentication</legend>
          <div className="flex flex-wrap gap-3" role="radiogroup" aria-label="Authentication">
            {(["none", "bearer", "oauth"] as AuthKind[]).map((k) => (
              <label key={k} className="inline-flex items-center gap-1.5 text-[13.5px]">
                <input type="radio" name="auth" checked={kind === k} onChange={() => setKind(k)} />
                {k === "none" ? "None" : k === "bearer" ? "Bearer token from a secret" : "Sign in (OAuth)"}
              </label>
            ))}
          </div>
          {kind === "bearer" && (
            <>
              <p className="muted m-0 text-[13px]">
                The token stays in a secret of this workspace. Lake reads it for each call and never shows it to anyone, a model or an agent.
              </p>
              <Field label="Secret mount" required error={fieldError(error, "auth/secret/mount_id")}>
                <select value={mountId} onChange={(e) => setMountId(e.target.value)}>
                  <option value="">{mounts.isLoading ? "Loading mounts…" : "Choose a mount"}</option>
                  {(mounts.data ?? []).map((m) => (
                    <option key={m.id} value={m.id}>
                      {m.name}
                    </option>
                  ))}
                </select>
              </Field>
              <Field label="Secret" required error={fieldError(error, "auth/secret/name")}>
                <input value={secretName} onChange={(e) => setSecretName(e.target.value)} placeholder="ticket-desk" autoComplete="off" spellCheck={false} />
              </Field>
              <Field label="Field" required help="The field of the secret that holds the token." error={fieldError(error, "auth/secret/field")}>
                <input value={field} onChange={(e) => setField(e.target.value)} autoComplete="off" spellCheck={false} />
              </Field>
            </>
          )}
          {kind === "oauth" && (
            <p className="muted m-0 text-[13px]">
              After you add it, sign in from the connection's page. Lake registers itself with the server's sign-in, uses PKCE, and keeps only the refresh token, as a secret of this workspace.
            </p>
          )}
        </fieldset>
        )}
        {tested && where === "remote" && (
          <Banner tone={tested.ok ? "ok" : "warn"} title={tested.ok ? `Reached ${tested.server?.name || "the server"}` : "Lake could not use the server"}>
            {tested.ok
              ? `${tested.tools?.length ?? 0} tools over ${tested.transport === "sse" ? "HTTP+SSE" : "Streamable HTTP"}, in ${tested.milliseconds ?? 0} ms. Nothing was kept.`
              : tested.error}
          </Banner>
        )}
        <SheetActions className="flex justify-end gap-2">{actions}</SheetActions>
      </form>
    </Modal>
  );
}

/** The fields of a server Lake runs here (lake#659): the package, its command and arguments, the
 * mount that holds the runner's token and the secret variables, and the environment. */
function StdioFields(props: {
  runtime: "node" | "python";
  setRuntime: (r: "node" | "python") => void;
  pkg: string;
  setPkg: (v: string) => void;
  command: string;
  setCommand: (v: string) => void;
  argsText: string;
  setArgsText: (v: string) => void;
  env: EnvRow[];
  setEnv: (rows: EnvRow[]) => void;
  mountId: string;
  setMountId: (v: string) => void;
  mounts: Row[];
  mountsLoading: boolean;
  error: unknown;
}) {
  const { env, setEnv } = props;
  const change = (index: number, patch: Partial<EnvRow>) => setEnv(env.map((row, i) => (i === index ? { ...row, ...patch } : row)));
  return (
    <>
      <p className="muted m-0 text-[13px]">
        Lake runs the package as an App of this workspace, metered like any App, and reaches it inside the cluster. The workspace needs internet access for the package to install.
      </p>
      <Field label="Runtime" required>
        <select value={props.runtime} onChange={(e) => props.setRuntime(e.target.value as "node" | "python")}>
          <option value="node">npm package (npx)</option>
          <option value="python">Python package (uvx)</option>
        </select>
      </Field>
      <Field label="Package" required help="Pin a version, so the App runs the same server every time it starts." error={fieldError(props.error, "stdio/package")}>
        <input
          value={props.pkg}
          onChange={(e) => props.setPkg(e.target.value)}
          placeholder={props.runtime === "python" ? "mcp-server-fetch==2025.4.7" : "@modelcontextprotocol/server-everything@2026.8.31"}
          autoComplete="off"
          spellCheck={false}
        />
      </Field>
      <Field label="Command" help="Only when the package's command is not named after it." error={fieldError(props.error, "stdio/command")}>
        <input value={props.command} onChange={(e) => props.setCommand(e.target.value)} autoComplete="off" spellCheck={false} />
      </Field>
      <Field label="Arguments" help="One per line." error={fieldError(props.error, "stdio/args")}>
        <textarea rows={2} className="font-mono" value={props.argsText} onChange={(e) => props.setArgsText(e.target.value)} spellCheck={false} />
      </Field>
      <Field label="Secret mount" required help="Lake keeps the runner's token here, and the secret variables below come from it." error={fieldError(props.error, "stdio/mount_id")}>
        <select value={props.mountId} onChange={(e) => props.setMountId(e.target.value)}>
          <option value="">{props.mountsLoading ? "Loading mounts…" : "Choose a mount"}</option>
          {props.mounts.map((m) => (
            <option key={m.id} value={m.id}>
              {m.name}
            </option>
          ))}
        </select>
      </Field>
      <fieldset className="stack m-0 border-0 p-0">
        <legend className="field-label">Environment</legend>
        {!env.length && <p className="muted m-0 text-[13px]">None. A variable is a plain value, or a field of a secret in the mount above.</p>}
        {env.map((row, index) => {
          const problem = row.name.trim() ? envNameProblem(row.name.trim()) : null;
          return (
            <div key={index} className="grid gap-2 sm:grid-cols-[minmax(0,1fr)_auto_minmax(0,1.4fr)_auto] sm:items-start">
              <Field label="Name" error={problem ?? fieldError(props.error, `stdio/env/${row.name.trim()}`)}>
                <input value={row.name} onChange={(e) => change(index, { name: e.target.value })} placeholder="GITHUB_TOKEN" autoComplete="off" spellCheck={false} />
              </Field>
              <Field label="From">
                <select value={row.source} onChange={(e) => change(index, { source: e.target.value as EnvRow["source"] })}>
                  <option value="value">A value</option>
                  <option value="secret">A secret</option>
                </select>
              </Field>
              {row.source === "value" ? (
                <Field label="Value" help="Not a credential: Lake refuses one here.">
                  <input value={row.value} onChange={(e) => change(index, { value: e.target.value })} autoComplete="off" spellCheck={false} />
                </Field>
              ) : (
                <div className="grid grid-cols-2 gap-2">
                  <Field label="Secret">
                    <input value={row.secret} onChange={(e) => change(index, { secret: e.target.value })} placeholder="github" autoComplete="off" spellCheck={false} />
                  </Field>
                  <Field label="Field">
                    <input value={row.field} onChange={(e) => change(index, { field: e.target.value })} placeholder="token" autoComplete="off" spellCheck={false} />
                  </Field>
                </div>
              )}
              <div className="sm:pt-6">
                <Button variant="ghost" onClick={() => setEnv(env.filter((_, i) => i !== index))} title={`Remove ${row.name || "this variable"}`}>
                  Remove
                </Button>
              </div>
            </div>
          );
        })}
        <div>
          <Button onClick={() => setEnv([...env, { name: "", source: "value", value: "", secret: "", field: "" }])}>Add variable</Button>
        </div>
      </fieldset>
    </>
  );
}
