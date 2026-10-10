/** Secret Mounts (`/secret-mounts?workspace_id=&organization_id=`): the secret mounts the viewer
 * may read in one workspace (web_ui_secret_mounts, serve_secret_mounts_page). The workspace is the
 * one named in the query, else the session's current workspace; the toolbar's choice of workspace
 * changes the query. */
import { useState } from "react";
import { useNavigate } from "react-router-dom";
import { KeyRound, Lock, Vault } from "lucide-react";
import { ApiError, useList, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { Button, DataTable, ErrorBlock, Empty, Mono, Page, SearchInput, Stat, StatRow, Toolbar } from "@/lib/ui";
import { NameCell, PanelEmpty, When, matches, useMetaTrail, useParam } from "@/pages/organizations/shared";

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
  useMetaTrail(mounts.data ? `${mounts.data.length} mounts` : null, "Secret Mounts");
  const rows = mounts.data?.filter((m) => matches(search, m.name, m.description, m.organization_id));
  const choose = (id: string) => {
    const next = new URLSearchParams();
    if (id) next.set("workspace_id", id);
    if (organizationId) next.set("organization_id", organizationId);
    navigate(`/secret-mounts${next.toString() ? `?${next}` : ""}`);
  };
  return (
    <Page
      eyebrow="Organization"
      title="Secret Mounts"
      sub="Organization-scoped static secrets and named encryption keys, kept in the workspace each mount was created in. Key material remains inside lake."
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
                <Empty icon={Lock} title="No secret mounts">
                  No secret mounts you can read in workspace <code>{workspace.id}</code> yet. An organization administrator creates them.
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
    </Page>
  );
}
