/** Secret Mounts (`/secret-mounts?workspace_id=&organization_id=`): the secret mounts the viewer
 * may read in one workspace (web_ui_secret_mounts, serve_secret_mounts_page). The workspace is the
 * one named in the query, else the session's current workspace. */
import { Link } from "react-router-dom";
import { ApiError, useList, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { DataTable, ErrorBlock, Page } from "@/lib/ui";
import { Dotted, PanelEmpty, When, useMetaTrail, useParam } from "@/pages/organizations/shared";

export default function SecretMountsPage() {
  const session = useSession();
  const asked = useParam("workspace_id");
  const organizationId = useParam("organization_id");
  const workspaceId = asked || session.workspaceId || "";
  // The workspace's space: the session knows it for every workspace the viewer can choose.
  const workspace = session.workspaces.find((w) => w.id === workspaceId);
  const unknown = !!asked && !workspace;
  const mounts = useList<Row>(
    workspace ? "list_secret_mounts" : null,
    { customer_id: workspace?.customer_id, workspace_id: workspace?.id, organization_id: organizationId, limit: 500 },
    { pick: "items", workspace: null },
  );
  useMetaTrail(mounts.data ? `${mounts.data.length} mounts` : null, "Secret Mounts");
  return (
    <Page eyebrow="control plane" title="Secret Mounts" sub="Organization-scoped static secrets and named encryption keys, kept in the workspace each mount was created in. Key material remains inside lake.">
      {unknown ? (
        <ErrorBlock error={new ApiError(400, "invalid_input", "workspace_id must name one of your workspaces", "/workspace_id")} />
      ) : !workspace ? (
        <PanelEmpty>Choose a workspace to see its secret mounts.</PanelEmpty>
      ) : (
        <DataTable
          q={mounts}
          what="secret mounts"
          rows={mounts.data}
          rowKey={(m) => m.id}
          empty={
            <PanelEmpty>
              No secret mounts you can read in workspace <code>{workspace.id}</code> yet. An organization administrator creates them.
            </PanelEmpty>
          }
          columns={[
            { key: "name", label: "Name", render: (m) => m.name },
            { key: "organization", label: "Organization", render: (m) => <code>{m.organization_id}</code> },
            { key: "workspace", label: "Workspace", render: (m) => <code>{m.workspace_id}</code> },
            { key: "description", label: "Description", render: (m) => m.description },
            { key: "created", label: "Created", render: (m) => <When value={m.created_at} /> },
            {
              key: "links",
              label: "",
              render: (m) => (
                <Dotted>
                  {[
                    <Link key="s" to={`/secrets?mount_id=${encodeURIComponent(m.id)}`}>
                      secrets
                    </Link>,
                    <Link key="k" to={`/encryption-keys?mount_id=${encodeURIComponent(m.id)}`}>
                      keys
                    </Link>,
                  ]}
                </Dotted>
              ),
            },
          ]}
        />
      )}
    </Page>
  );
}
