/** Members (`/members?organization_id=&team_id=`): an organization's or team's memberships with
 * their attestations (web_ui_memberships, serve_members_page). */
import { Link, Navigate } from "react-router-dom";
import { useList, type Row } from "@/lib/api";
import { DataTable, Page } from "@/lib/ui";
import { PanelEmpty, RoleBadge, useMetaTrail, useParam } from "./shared";

export default function MembersPage() {
  const organizationId = useParam("organization_id");
  const teamId = useParam("team_id");
  const memberships = useList<Row>(organizationId ? "list_memberships" : null, { organization_id: organizationId, team_id: teamId, limit: 500 }, { pick: "items" });
  useMetaTrail(teamId || organizationId || null, "Members");
  if (!organizationId) return <Navigate to="/organizations" replace />;
  return (
    <Page eyebrow="authorization" title="Members" sub="Every role is backed by an org_membership/1 attestation.">
      <DataTable
        q={memberships}
        what="members"
        rows={memberships.data}
        rowKey={(m) => m.id}
        empty={
          <PanelEmpty>
            No members yet. <Link to={`/organization?id=${encodeURIComponent(organizationId)}&tab=invitations`}>Invite people</Link> to add them.
          </PanelEmpty>
        }
        columns={[
          { key: "principal", label: "Principal", render: (m) => <code>{m.principal_taap_id}</code> },
          { key: "type", label: "Type", render: (m) => m.principal_type },
          { key: "role", label: "Role", render: (m) => <RoleBadge role={m.role} /> },
          { key: "attestation", label: "Attestation", render: (m) => <code>{m.attestation_digest}</code> },
          { key: "granted", label: "Granted by", render: (m) => <code>{m.granted_by_taap_id}</code> },
        ]}
      />
    </Page>
  );
}
