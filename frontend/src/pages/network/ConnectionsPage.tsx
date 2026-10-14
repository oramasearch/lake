/** Network → Connections (`/w/<id>/network/connections`, lake#729, docs/ui/network.md): the paths
 * declared between this workspace's apps and services, each with its ports, its own credential when
 * an app reaches a service, and whether the cluster has applied it. "Connect" declares one
 * (`create_network_connection`), a scoped credential can be rotated
 * (`rotate_network_connection_credential`) and a connection removed (`remove_network_connection`).
 * Not Data → Connections: those are the databases a workspace reads for datasets. */
import { useState } from "react";
import { Link } from "react-router-dom";
import { Cable, Plus } from "lucide-react";
import { act, type Row } from "@/lib/api";
import { relative } from "@/lib/format";
import { ConfirmButton, Drawer } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Actions, Button, Chip, DataTable, Empty, Field, Form, Mono, Toolbar, fieldError } from "@/lib/ui";
import { useAsync, useWsPath } from "@/pages/train/workspace";
import { connectionInput, credentialAllowed, nameOf, portsText, present, scopeText, type ConnectionForm, type Names } from "./network";
import { Endpoint, NetworkPage, RecordState, useNames, useNetworkRead, useScope } from "./parts";

export default function ConnectionsPage() {
  const ws = useWsPath();
  const [adding, setAdding] = useState(false);
  const [history, setHistory] = useState(false);
  const q = useNetworkRead<{ connections?: Row[] }>("list_network_connections");
  const { names, apps, services } = useNames();
  const all = q.data?.connections ?? [];
  const rows = present(all, history);
  const withdrawn = all.length - present(all, false).length;
  return (
    <NetworkPage
      title="Connections"
      sub={
        <>
          Which of this workspace's apps and services may reach which, on which ports, and with what credential. The databases the workspace reads for datasets are under{" "}
          <Link to={ws("/connections")}>Data → Connections</Link>.
        </>
      }
      actions={
        <Button variant="primary" onClick={() => setAdding(true)}>
          <Plus size={14} aria-hidden /> Connect an app
        </Button>
      }
    >
      {withdrawn > 0 && (
        <Toolbar>
          <label className="inline-flex items-center gap-2 text-[12.5px]">
            <input type="checkbox" checked={history} onChange={(e) => setHistory(e.target.checked)} /> Show {withdrawn} removed
          </label>
        </Toolbar>
      )}
      <DataTable<Row>
        q={q}
        what="connections"
        caption="Network connections"
        rows={q.data ? rows : undefined}
        rowKey={(c) => String(c.id)}
        initialSort={{ key: "created", dir: "descending" }}
        empty={
          <Empty icon={Cable} title="No Connections Yet" action={<Button variant="primary" onClick={() => setAdding(true)}>Connect an app</Button>}>
            Nothing in this workspace reaches anything else until a connection says so. Connect an app to the service it uses.
          </Empty>
        }
        columns={[
          { key: "from", label: "From", sort: (c) => nameOf(names, String(c.source_kind), String(c.source_id)), render: (c) => <Endpoint names={names} kind={String(c.source_kind)} id={String(c.source_id)} /> },
          { key: "to", label: "To", sort: (c) => nameOf(names, String(c.target_kind), String(c.target_id)), render: (c) => <Endpoint names={names} kind={String(c.target_kind)} id={String(c.target_id)} /> },
          { key: "ports", label: "Ports", render: (c) => <span className="font-mono text-[12px]">{portsText(c.ports)}</span> },
          {
            key: "credential",
            label: "Credential",
            render: (c) =>
              c.credential_scope ? (
                <div className="min-w-0">
                  <Chip>{scopeText(c.credential_scope)}</Chip>
                  {(c.status as Row | null)?.credential_secret ? (
                    <div className="mt-1 text-[11.5px] text-ink-3">
                      Secret <Mono>{String((c.status as Row).credential_secret)}</Mono>
                    </div>
                  ) : null}
                </div>
              ) : (
                <span className="muted">None</span>
              ),
          },
          { key: "status", label: "Status", sort: (c) => String((c.status as Row | null)?.phase ?? ""), render: (c) => <RecordState record={c} /> },
          { key: "created", label: "Created", sort: (c) => String(c.created_at), render: (c) => <span className="font-mono text-[12px] text-ink-2" title={String(c.created_at)}>{relative(c.created_at)}</span> },
          { key: "actions", label: "", className: "text-right", render: (c) => (c.desired === "absent" ? null : <RowActions connection={c} names={names} />) },
        ]}
      />
      {adding && <Connect names={names} apps={apps} services={services} onClose={() => setAdding(false)} />}
    </NetworkPage>
  );
}

function RowActions({ connection, names }: { connection: Row; names: Names }) {
  const scope = useScope();
  const toast = useToast();
  const from = nameOf(names, String(connection.source_kind), String(connection.source_id));
  const to = nameOf(names, String(connection.target_kind), String(connection.target_id));
  const input = { ...scope.input, connection_id: String(connection.id) };
  return (
    <Actions className="justify-end">
      {connection.credential_scope ? (
        <ConfirmButton
          variant="ghost"
          title="Rotate the credential?"
          confirmLabel="Rotate"
          confirm={<>The cluster replaces the credential {from} uses for {to} and rewrites the same secret. The old credential stops working once it has.</>}
          onConfirm={async () => {
            await act("rotate_network_connection_credential", input);
            toast(`Rotating the credential for ${from}`);
          }}
        >
          Rotate credential
        </ConfirmButton>
      ) : null}
      <ConfirmButton
        variant="ghost"
        danger
        title={`Remove the connection from ${from} to ${to}?`}
        confirmLabel="Remove"
        confirm={<>{from} can no longer reach {to} once the cluster applies it{connection.credential_scope ? ", and its credential is revoked" : ""}. The connection stays in the history.</>}
        onConfirm={async () => {
          await act("remove_network_connection", input);
          toast(`Removed the connection to ${to}`);
        }}
      >
        Remove
      </ConfirmButton>
    </Actions>
  );
}

/** "Connect an app": what connects, what it reaches, the ports and, from an app to a service, the
 * credential it gets. */
function Connect({ names, apps, services, onClose }: { names: Names; apps: Row[]; services: Row[]; onClose: () => void }) {
  const scope = useScope();
  const toast = useToast();
  const save = useAsync();
  const [form, setForm] = useState<ConnectionForm>({ sourceKind: "app", sourceId: "", targetKind: "service", targetId: "", ports: "", access: "", schema: "" });
  const set = (patch: Partial<ConnectionForm>) => setForm((f) => ({ ...f, ...patch }));
  const options = (kind: string) => (kind === "app" ? apps : services).map((r) => ({ id: String(r.id), label: String(r.name || r.id) }));
  const credential = credentialAllowed(form.sourceKind, form.targetKind);
  return (
    <Drawer
      title="Connect an App"
      subtitle="Declare one path. The cluster opens exactly this and nothing else."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="network-connect" loading={save.busy}>
            Connect
          </Button>
        </>
      }
    >
      <Form
        id="network-connect"
        error={save.error}
        onSubmit={() =>
          save.run(async () => {
            await act("create_network_connection", { ...scope.input, ...connectionInput(form) });
            toast(`Connected ${nameOf(names, form.sourceKind, form.sourceId)} to ${nameOf(names, form.targetKind, form.targetId)}`);
            onClose();
          })
        }
      >
        <Field label="From" error={fieldError(save.error, "source_id")}>
          <div className="flex gap-2">
            <select aria-label="Kind" value={form.sourceKind} onChange={(e) => set({ sourceKind: e.target.value, sourceId: "" })} className="w-32">
              <option value="app">App</option>
              <option value="service">Service</option>
            </select>
            <Picker label="From" value={form.sourceId} options={options(form.sourceKind)} onChange={(id) => set({ sourceId: id })} kind={form.sourceKind} />
          </div>
        </Field>
        <Field label="To" error={fieldError(save.error, "target_id")}>
          <div className="flex gap-2">
            <select aria-label="Kind" value={form.targetKind} onChange={(e) => set({ targetKind: e.target.value, targetId: "", access: e.target.value === "service" ? form.access : "" })} className="w-32">
              <option value="service">Service</option>
              <option value="app">App</option>
            </select>
            <Picker label="To" value={form.targetId} options={options(form.targetKind)} onChange={(id) => set({ targetId: id })} kind={form.targetKind} />
          </div>
        </Field>
        <Field label="Ports" help="Separated by commas. Leave empty for every port it serves." error={fieldError(save.error, "ports")}>
          <input inputMode="numeric" placeholder="5432" value={form.ports} onChange={(e) => set({ ports: e.target.value })} />
        </Field>
        {credential && (
          <>
            <Field label="Credential" help="Its own role on the service, made for this connection and revoked with it. Lake never shows its value." error={fieldError(save.error, "credential_scope")}>
              <select value={form.access} onChange={(e) => set({ access: e.target.value as ConnectionForm["access"] })}>
                <option value="">None: reach the service only</option>
                <option value="read">Read only</option>
                <option value="write">Read and write</option>
              </select>
            </Field>
            {form.access && (
              <Field label="Schema" help="Optional: limit the credential to one schema.">
                <input placeholder="analytics" value={form.schema} onChange={(e) => set({ schema: e.target.value })} />
              </Field>
            )}
          </>
        )}
      </Form>
    </Drawer>
  );
}

function Picker({ label, value, options, onChange, kind }: { label: string; value: string; options: { id: string; label: string }[]; onChange: (id: string) => void; kind: string }) {
  if (options.length === 0) return <span className="muted self-center text-[12.5px]">This workspace has no {kind === "app" ? "apps" : "services"} yet.</span>;
  return (
    <select aria-label={label} value={value} onChange={(e) => onChange(e.target.value)} className="min-w-0 flex-1">
      <option value="">Choose…</option>
      {options.map((o) => (
        <option key={o.id} value={o.id}>
          {o.label}
        </option>
      ))}
    </select>
  );
}
