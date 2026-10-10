/** Secret Mounts (`/secret-mounts?workspace_id=&organization_id=`): the secret mounts the viewer
 * may read in one workspace (web_ui_secret_mounts, serve_secret_mounts_page), and New Mount
 * (`create_secret_mount`, docs/ui/secrets.md). The workspace is the one named in the query, else
 * the session's current workspace; the toolbar's choice of workspace changes the query. */
import { useState } from "react";
import { useNavigate } from "react-router-dom";
import { KeyRound, Lock, Plus, Vault } from "lucide-react";
import { ApiError, act, useList, useRead, type Row } from "@/lib/api";
import { Modal, SheetActions } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { Banner, Button, DataTable, ErrorBlock, Empty, Field, Form, Mono, Page, SearchInput, Stat, StatRow, Toolbar, fieldError } from "@/lib/ui";
import { HANDLE_PATTERN, NameCell, PanelEmpty, When, matches, useMetaTrail, useParam } from "@/pages/organizations/shared";

export default function SecretMountsPage() {
  const session = useSession();
  const navigate = useNavigate();
  const asked = useParam("workspace_id");
  const organizationId = useParam("organization_id");
  const workspaceId = asked || session.workspaceId || "";
  // The workspace's customer: the session knows it for every workspace the viewer can choose.
  const workspace = session.workspaces.find((w) => w.id === workspaceId);
  const unknown = !!asked && !workspace;
  const mounts = useList<Row>(
    workspace ? "list_secret_mounts" : null,
    { customer_id: workspace?.customer_id, workspace_id: workspace?.id, organization_id: organizationId, limit: 500 },
    { pick: "items", workspace: null },
  );
  const [search, setSearch] = useState("");
  const [creating, setCreating] = useState(false);
  useMetaTrail(mounts.data ? `${mounts.data.length} mounts` : null, "Secret Mounts");
  const rows = mounts.data?.filter((m) => matches(search, m.name, m.description, m.organization_id));
  const choose = (id: string) => {
    const next = new URLSearchParams();
    if (id) next.set("workspace_id", id);
    if (organizationId) next.set("organization_id", organizationId);
    navigate(`/secret-mounts${next.toString() ? `?${next}` : ""}`);
  };
  const newMount = (
    <Button variant="primary" onClick={() => setCreating(true)} disabled={session.workspaces.length === 0}>
      <Plus size={14} aria-hidden /> New mount
    </Button>
  );
  return (
    <Page
      eyebrow="Organization"
      title="Secret Mounts"
      sub="Organization-scoped static secrets and named encryption keys, kept in the workspace each mount was created in. Key material remains inside lake."
      actions={newMount}
    >
      {session.workspaces.length > 1 && (
        <Toolbar>
          <label className="inline-flex items-center gap-2 text-[13px] text-ink-2">
            Workspace
            <select aria-label="Workspace" value={workspace?.id ?? ""} onChange={(e) => choose(e.target.value)}>
              {!workspace && <option value="">Choose a workspace</option>}
              {session.workspaces.map((w) => (
                <option key={w.id} value={w.id}>
                  {w.name}
                </option>
              ))}
            </select>
          </label>
        </Toolbar>
      )}
      {unknown ? (
        <ErrorBlock error={new ApiError(400, "invalid_input", "workspace_id must name one of your workspaces", "/workspace_id")} />
      ) : !workspace ? (
        <Empty icon={Vault} title="Choose a workspace">
          Choose a workspace to see its secret mounts.
        </Empty>
      ) : (
        <>
          {mounts.data && mounts.data.length > 0 && (
            <StatRow cols={2}>
              <Stat label="Mounts" value={mounts.data.length} icon={Vault} sub={`in ${workspace.name}`} />
              <Stat label="Organizations" value={new Set(mounts.data.map((m) => m.organization_id)).size} sub="own these mounts" />
            </StatRow>
          )}
          <Toolbar>
            <SearchInput value={search} onChange={setSearch} placeholder="Search mounts…" />
          </Toolbar>
          <DataTable
            q={mounts}
            what="secret mounts"
            rows={rows}
            rowKey={(m) => m.id}
            empty={
              mounts.data?.length ? (
                <PanelEmpty title="No mount matches">Clear the search to see every mount.</PanelEmpty>
              ) : (
                <Empty icon={Lock} title="No secret mounts" action={newMount}>
                  No secret mounts you can read in {workspace.name} yet. A mount holds an organization's secrets and keys; an organization administrator creates one.
                </Empty>
              )
            }
            columns={[
              { key: "name", label: "Mount", sort: (m) => m.name, render: (m) => <NameCell to={`/secrets?mount_id=${encodeURIComponent(m.id)}`} name={m.name} sub={m.description} /> },
              { key: "organization", label: "Organization", render: (m) => <Mono short={14}>{m.organization_id}</Mono> },
              { key: "workspace", label: "Workspace", render: (m) => <span title={m.workspace_id}>{session.workspaces.find((w) => w.id === m.workspace_id)?.name ?? <Mono short={12}>{m.workspace_id}</Mono>}</span> },
              { key: "created", label: "Created", sort: (m) => m.created_at, render: (m) => <When value={m.created_at} /> },
              {
                key: "links",
                label: <span className="sr-only">Links</span>,
                render: (m) => (
                  <span className="flex flex-wrap gap-1.5">
                    <Button to={`/secrets?mount_id=${encodeURIComponent(m.id)}`}>
                      <Lock size={14} aria-hidden /> Secrets
                    </Button>
                    <Button to={`/encryption-keys?mount_id=${encodeURIComponent(m.id)}`}>
                      <KeyRound size={14} aria-hidden /> Keys
                    </Button>
                  </span>
                ),
              },
            ]}
          />
        </>
      )}
      {creating && <NewMount workspaceId={workspace?.id ?? ""} organizationId={organizationId} onClose={() => setCreating(false)} />}
    </Page>
  );
}

/** New Mount: one workspace, chosen here and never changed, and the organization that owns it. */
function NewMount({ workspaceId: initialWorkspace, organizationId: initialOrganization, onClose }: { workspaceId: string; organizationId: string; onClose: () => void }) {
  const session = useSession();
  const navigate = useNavigate();
  const toast = useToast();
  const [workspaceId, setWorkspaceId] = useState(initialWorkspace || session.workspaces[0]?.id || "");
  const [organizationId, setOrganizationId] = useState(initialOrganization);
  const [name, setName] = useState("");
  const [description, setDescription] = useState("");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const workspace = session.workspaces.find((w) => w.id === workspaceId);
  const organizations = useList<Row>("list_organizations", { limit: 500 }, { pick: "items", workspace: null });
  // An organization's own workspaces belong to it: offer it first.
  const customer = useRead<Row>(workspace ? "get_customer" : null, { customer_id: workspace?.customer_id }, { workspace: null });
  const owner = customer.data?.organization_id as string | undefined;
  const chosen = organizationId || owner || "";
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      const input: Row = { customer_id: workspace?.customer_id ?? "", workspace_id: workspaceId, organization_id: chosen, name };
      if (description.trim()) input.description = description.trim();
      const mount = await act<Row>("create_secret_mount", input);
      toast(`Created mount ${mount.name}`);
      onClose();
      navigate(`/secrets?mount_id=${encodeURIComponent(mount.id)}`);
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const fieldFault = ["workspace_id", "customer_id", "organization_id", "name", "description"].some((f) => fieldError(error, f));
  return (
    <Modal title="New Secret Mount" subtitle="A mount holds secrets and encryption keys for one organization, in one workspace." onClose={onClose} sheet>
      <Form onSubmit={submit} error={fieldFault ? null : error}>
        <Field label="Workspace" required help="Its secrets and keys are sealed in this workspace's Zetta. It cannot be changed later." error={fieldError(error, "workspace_id") ?? fieldError(error, "customer_id")}>
          <select value={workspaceId} onChange={(e) => setWorkspaceId(e.target.value)}>
            {session.viewer.spaces.map((space) =>
              space.workspaces.length ? (
                <optgroup key={space.id} label={space.label}>
                  {space.workspaces.map((w) => (
                    <option key={w.id} value={w.id}>
                      {w.name}
                    </option>
                  ))}
                </optgroup>
              ) : null,
            )}
          </select>
        </Field>
        {organizations.isError ? (
          <ErrorBlock error={organizations.error} retry={() => void organizations.refetch()} what="your organizations" />
        ) : organizations.data && organizations.data.length === 0 ? (
          <Banner tone="warn" title="No organization">
            A mount belongs to an organization. Create one under Organization → Organizations first.
          </Banner>
        ) : (
          <Field label="Organization" required help="Its administrators create mounts; access is granted on the mount through it." error={fieldError(error, "organization_id")}>
            <select value={chosen} onChange={(e) => setOrganizationId(e.target.value)}>
              {!chosen && <option value="">Choose an organization</option>}
              {(organizations.data ?? []).map((o) => (
                <option key={o.id} value={o.id}>
                  {o.display_name || o.name || o.id}
                </option>
              ))}
            </select>
          </Field>
        )}
        <Field label="Name" required help="Lowercase letters, digits and dashes; unique in this workspace and organization." error={fieldError(error, "name")}>
          <input value={name} onChange={(e) => setName(e.target.value)} pattern={HANDLE_PATTERN} placeholder="payments" autoComplete="off" spellCheck={false} />
        </Field>
        <Field label="Description" help="Optional." error={fieldError(error, "description")}>
          <input value={description} onChange={(e) => setDescription(e.target.value)} placeholder="Payment service credentials" />
        </Field>
        <SheetActions className="flex justify-end gap-2">
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" variant="primary" loading={busy} disabled={!workspace || !chosen || !name}>
            Create mount
          </Button>
        </SheetActions>
      </Form>
    </Modal>
  );
}
