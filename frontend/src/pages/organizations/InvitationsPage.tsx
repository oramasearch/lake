/** Invitations (`/invitations?organization_id=&team_id=`): the invitations an organization or team
 * sent (web_ui_invitations, serve_invitations_page). */
import { Link, Navigate } from "react-router-dom";
import { useList, type Row } from "@/lib/api";
import { DataTable, Page, Status } from "@/lib/ui";
import { PanelEmpty, RoleBadge, When, useMetaTrail, useParam } from "./shared";

export default function InvitationsPage() {
  const organizationId = useParam("organization_id");
  const teamId = useParam("team_id");
  const invitations = useList<Row>(organizationId ? "list_invitations" : null, { organization_id: organizationId, team_id: teamId, limit: 500 }, { pick: "items" });
  useMetaTrail(teamId || organizationId || null, "Invitations");
  if (!organizationId) return <Navigate to="/organizations" replace />;
  return (
    <Page eyebrow="organization" title="Invitations" sub="Email and handle invitations expire and become attested memberships.">
      <DataTable
        q={invitations}
        what="invitations"
        rows={invitations.data}
        rowKey={(i) => i.id}
        empty={
          <PanelEmpty>
            No invitations yet. <Link to={`/organization?id=${encodeURIComponent(organizationId)}&tab=invitations`}>Invite someone</Link> by email address or handle.
          </PanelEmpty>
        }
        columns={[
          { key: "invitee", label: "Invitee", render: (i) => `${i.invitee_type}: ${i.invitee}` },
          { key: "role", label: "Role", render: (i) => <RoleBadge role={i.role} /> },
          { key: "status", label: "Status", render: (i) => <Status value={i.status} /> },
          { key: "expires", label: "Expires", render: (i) => <When value={i.expires_at} />, sort: (i) => i.expires_at },
        ]}
      />
    </Page>
  );
}
