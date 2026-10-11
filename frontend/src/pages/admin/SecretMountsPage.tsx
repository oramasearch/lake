/** Secret Mounts (`/secret-mounts?workspace_id=&organization_id=`): the secret mounts the viewer
 * may read in one workspace (web_ui_secret_mounts, serve_secret_mounts_page), and New Mount
 * (`create_secret_mount`, docs/ui/secrets.md). The workspace is the one named in the query, else
 * the session's current workspace; the toolbar's choice of workspace changes the query. A mount's
 * owner is the account that owns the workspace or an organization inside that account's space
 * (secretOwners.ts). */
import { useMemo, useState } from "react";
import { useQueries } from "@tanstack/react-query";
import { useNavigate } from "react-router-dom";
import { KeyRound, Lock, Plus, Trash2, Vault } from "lucide-react";
import { ApiError, act, read, useList, useRead, type Row } from "@/lib/api";
import { secretMountLines } from "@/lib/deletion";
import { DeletionButton } from "@/lib/deletionDialog";
import { Modal, SheetActions } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { Button, DataTable, ErrorBlock, Empty, Field, Form, Mono, Page, SearchInput, Stat, StatRow, Toolbar, fieldError } from "@/lib/ui";
import { HANDLE_PATTERN, NameCell, PanelEmpty, When, matches, useMetaTrail, useParam } from "@/pages/organizations/shared";
import { administeredOrganizationIds, mountOwnerLabel, ownerOptions, type AdministeredOrganization } from "./secretOwners";

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
  // Organization names for the Owner column; the account is named from the session.
  const organizations = useList<Row>("list_organizations", { limit: 500 }, { pick: "items", workspace: null });
  const organizationsById = useMemo(() => new Map((organizations.data ?? []).map((o) => [o.id as string, o as { id: string; display_name?: string; name?: string }])), [organizations.data]);
  const space = session.viewer.spaces.find((s) => s.id === workspace?.customer_id);
  const ownerOf = (m: Row) => mountOwnerLabel(m, organizationsById, space);
  const rows = mounts.data?.filter((m) => matches(search, m.name, m.description, m.organization_id, ownerOf(m)));
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
      sub="Static secrets and named encryption keys, kept in the workspace each mount was created in. Your account or one of your organizations owns each mount. Key material remains inside Lake."
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
              <Stat label="Owners" value={new Set(mounts.data.map((m) => m.organization_id ?? "account")).size} sub="own these mounts" />
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
                  No secret mounts you can read in {workspace.name} yet. A mount holds secrets and keys for your account or one of your organizations.
                </Empty>
              )
            }
            columns={[
              { key: "name", label: "Mount", sort: (m) => m.name, render: (m) => <NameCell to={`/secrets?mount_id=${encodeURIComponent(m.id)}`} name={m.name} sub={m.description} /> },
              { key: "owner", label: "Owner", sort: (m) => ownerOf(m), render: (m) => <span title={m.organization_id ?? m.customer_id}>{ownerOf(m)}</span> },
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
                    <DeletionButton
                      variant="ghost"
                      capability="delete_secret_mount"
                      input={{ mount_id: m.id }}
                      workspace={null}
                      title={`Delete ${m.name}`}
                      confirmLabel="Delete mount"
                      intro="Deleting the mount removes everything in it from its workspace. It cannot be undone."
                      typed={(plan) => String(plan.name || m.name)}
                      sendConfirm
                      lines={secretMountLines}
                      details={(plan) => <SecretNames names={plan.secrets} />}
                      done={`Deleted ${m.name}`}
                    >
                      <Trash2 size={14} aria-hidden /> Delete
                    </DeletionButton>
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

/** The names of the secrets a mount deletion removes, folded. */
function SecretNames({ names }: { names: unknown }) {
  const list = Array.isArray(names) ? (names as string[]) : [];
  if (list.length === 0) return null;
  return (
    <details className="mt-2">
      <summary className="cursor-pointer text-[12.5px] text-ink-2">Every secret by name</summary>
      <ul className="mt-2 space-y-1 pl-4 text-[13px]">
        {list.map((name) => (
          <li key={name}>
            <code>{name}</code>
          </li>
        ))}
      </ul>
    </details>
  );
}

/** The Owner select's value for "the account that owns the workspace" (no organization). */
const ACCOUNT_OWNER = "account";

/** New Mount: one workspace, chosen here and never changed, and its owner: the account that owns
 * the workspace (the default) or an organization the person administers inside that space. */
function NewMount({ workspaceId: initialWorkspace, organizationId: initialOrganization, onClose }: { workspaceId: string; organizationId: string; onClose: () => void }) {
  const session = useSession();
  const navigate = useNavigate();
  const toast = useToast();
  const [workspaceId, setWorkspaceId] = useState(initialWorkspace || session.workspaces[0]?.id || "");
  // null: the workspace's owner, whatever workspace is chosen.
  const [ownerChoice, setOwnerChoice] = useState<string | null>(initialOrganization || null);
  const [name, setName] = useState("");
  const [description, setDescription] = useState("");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const workspace = session.workspaces.find((w) => w.id === workspaceId);
  const space = session.viewer.spaces.find((s) => s.id === workspace?.customer_id);
  // An organization's own workspaces belong to it: its customer names it.
  const customer = useRead<Row>(workspace ? "get_customer" : null, { customer_id: workspace?.customer_id }, { workspace: null });
  const ownerOrganization = (customer.data?.organization_id as string | null | undefined) ?? null;
  // The organizations the person administers: their own owner or admin memberships. The operator
  // has no memberships and administers every organization.
  const person = !!session.viewer.user_id;
  const organizations = useList<Row>("list_organizations", { limit: 500 }, { pick: "items", workspace: null });
  const memberships = useList<Row>(person ? "list_memberships" : null, { limit: 500 }, { pick: "items", workspace: null });
  const administeredIds = person ? administeredOrganizationIds(memberships.data ?? []) : new Set((organizations.data ?? []).map((o) => o.id as string));
  const administeredRows = (organizations.data ?? []).filter((o) => administeredIds.has(o.id));
  // Each one's own space, so an organization of another space is not offered.
  const spaces = useQueries({
    queries: administeredRows.map((o) => ({
      queryKey: ["get_organization", o.id],
      queryFn: () => read<Row>("get_organization", { id: o.id }, { workspace: null }),
      staleTime: 60_000,
    })),
  });
  const administered: AdministeredOrganization[] = administeredRows.map((o, index) => ({
    id: o.id,
    display_name: o.display_name,
    name: o.name,
    customer_id: spaces[index]?.data ? ((spaces[index].data.customer_id as string | null) ?? null) : undefined,
  }));
  const options = ownerOptions({ space, workspaceCustomerId: workspace?.customer_id ?? "", ownerOrganizationId: ownerOrganization, administered });
  const chosen = ownerChoice !== null && options.some((o) => o.organizationId === ownerChoice) ? ownerChoice : (options[0]?.organizationId ?? "");
  const ownerKnown = !workspace || !customer.isLoading;
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      const input: Row = { customer_id: workspace?.customer_id ?? "", workspace_id: workspaceId, name };
      if (chosen) input.organization_id = chosen;
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
    <Modal title="New Secret Mount" subtitle="A mount holds secrets and encryption keys in one workspace, for your account or one of your organizations." onClose={onClose} sheet>
      <Form onSubmit={submit} error={fieldFault ? null : error}>
        <Field label="Workspace" required help="Its secrets and keys are sealed in this workspace. It cannot be changed later." error={fieldError(error, "workspace_id")}>
          <select
            value={workspaceId}
            onChange={(e) => {
              setWorkspaceId(e.target.value);
              setOwnerChoice(null);
            }}
          >
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
        <Field
          label="Owner"
          required
          help="Who administers the mount and grants access to it: the account that owns the workspace, or an organization you administer in that account's space."
          error={fieldError(error, "organization_id") ?? fieldError(error, "customer_id")}
        >
          {/* The account's option needs a non-empty value: a required <select> whose value is "" is
              invalid, and the browser would refuse to submit (lake#619). */}
          <select value={chosen || ACCOUNT_OWNER} onChange={(e) => setOwnerChoice(e.target.value === ACCOUNT_OWNER ? "" : e.target.value)} disabled={!workspace}>
            {options.map((o) => (
              <option key={o.organizationId || ACCOUNT_OWNER} value={o.organizationId || ACCOUNT_OWNER}>
                {o.label}
              </option>
            ))}
          </select>
        </Field>
        <Field label="Name" required help="Lowercase letters, digits and dashes; unique in this workspace for its owner." error={fieldError(error, "name")}>
          <input value={name} onChange={(e) => setName(e.target.value)} pattern={HANDLE_PATTERN} placeholder="payments" autoComplete="off" spellCheck={false} />
        </Field>
        <Field label="Description" help="Optional." error={fieldError(error, "description")}>
          <input value={description} onChange={(e) => setDescription(e.target.value)} placeholder="Payment service credentials" />
        </Field>
        <SheetActions className="flex justify-end gap-2">
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" variant="primary" loading={busy} disabled={!workspace || !ownerKnown || !name}>
            Create mount
          </Button>
        </SheetActions>
      </Form>
    </Modal>
  );
}
