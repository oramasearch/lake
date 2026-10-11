/** New MCP connection (lake#652, docs/ui/mcp-connections.md): a `Modal sheet`. The server's URL, a
 * name (its prefix shown as the person types), the transport, and how Lake authenticates: none, a
 * bearer token kept in a secret of this workspace (a mount, the secret's name and its field), or an
 * OAuth sign-in a person does on the connection's page. "Test" connects without keeping anything;
 * "Add connection" keeps it and discovers its tools. The token itself is never typed here: it is a
 * secret, put in a secret mount first. */
import { useId, useState } from "react";
import { act, useList, type Row } from "@/lib/api";
import { Banner, Button, Field, FormError, fieldError } from "@/lib/ui";
import { Modal, SheetActions } from "@/lib/overlay";
import { useWorkspace } from "@/pages/train/workspace";
import { prefixOf, type McpConnection } from "./logic";

type AuthKind = "none" | "bearer" | "oauth";
type Tested = { ok: boolean; status?: string; error?: string; server?: { name?: string; version?: string }; transport?: string; tools?: string[]; milliseconds?: number };

export function NewMcpConnection({ onClose, onCreated }: { onClose: () => void; onCreated: (c: McpConnection) => void }) {
  const { customerId, workspaceId } = useWorkspace();
  const formId = useId();
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
  const mounts = useList<Row>(kind === "bearer" ? "list_secret_mounts" : null, { customer_id: customerId, workspace_id: workspaceId, limit: 200 }, { pick: "items" });
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

  const create = async () => {
    setBusy(true);
    setError(null);
    try {
      onCreated(await act<McpConnection>("create_mcp_connection", { customer_id: customerId, workspace_id: workspaceId, name: name.trim(), url: url.trim(), transport, auth: auth() }));
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };

  const ready = !!name.trim() && !!url.trim() && (kind !== "bearer" || (!!mountId && !!secretName.trim() && !!field.trim()));
  const actions = (
    <>
      <Button onClick={onClose}>Cancel</Button>
      <Button onClick={() => void test()} loading={testing} disabled={!url.trim() || kind === "oauth" || (kind === "bearer" && !ready)}>
        Test
      </Button>
      <Button variant="primary" type="submit" form={formId} loading={busy} disabled={!ready}>
        Add connection
      </Button>
    </>
  );

  return (
    <Modal title="New MCP Connection" subtitle="A remote MCP server this workspace will use." onClose={onClose} sheet width={600}>
      <form
        id={formId}
        className="stack"
        onSubmit={(e) => {
          e.preventDefault();
          if (ready) void create();
        }}
      >
        <FormError error={error && !fieldError(error, "url") && !fieldError(error, "name") && !fieldError(error, "auth") ? error : null} />
        <Field label="Server URL" required help="Streamable HTTP or HTTP+SSE, over https. Lake connects only to public addresses." error={fieldError(error, "url")}>
          <input value={url} onChange={(e) => setUrl(e.target.value)} placeholder="https://mcp.example.com/mcp" inputMode="url" autoComplete="off" spellCheck={false} />
        </Field>
        <Field
          label="Name"
          required
          help={prefix ? <>Its tools are named <code>{prefix}__&lt;tool&gt;</code> outside the connection.</> : "How people and agents will know it."}
          error={fieldError(error, "name")}
        >
          <input value={name} onChange={(e) => setName(e.target.value)} placeholder="Ticket Desk" autoComplete="off" />
        </Field>
        <Field label="Transport" help="Automatic tries Streamable HTTP, then the older HTTP+SSE.">
          <select value={transport} onChange={(e) => setTransport(e.target.value)}>
            <option value="auto">Automatic</option>
            <option value="streamable_http">Streamable HTTP</option>
            <option value="sse">HTTP+SSE (older servers)</option>
          </select>
        </Field>
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
        {tested && (
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
