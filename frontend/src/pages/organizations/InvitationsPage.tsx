/** Invitations (`/invitations?organization_id=&team_id=`): the invitations an organization or team
 * sent (web_ui_invitations, serve_invitations_page). */
import { useState } from "react";
import { Navigate } from "react-router-dom";
import { UserPlus } from "lucide-react";
import { useList, type Row } from "@/lib/api";
import { Button, DataTable, FilterChips, Page, SearchInput, Status, Toolbar } from "@/lib/ui";
import { NameCell, PanelEmpty, RoleBadge, When, countBy, matches, useMetaTrail, useParam } from "./shared";

export default function InvitationsPage() {
  const organizationId = useParam("organization_id");
  const teamId = useParam("team_id");
  const invitations = useList<Row>(organizationId ? "list_invitations" : null, { organization_id: organizationId, team_id: teamId, limit: 500 }, { pick: "items" });
  const [search, setSearch] = useState("");
  const [status, setStatus] = useState<string | null>(null);
  useMetaTrail(teamId || organizationId || null, "Invitations");
  if (!organizationId) return <Navigate to="/organizations" replace />;
  const invite = `/organization?id=${encodeURIComponent(organizationId)}&tab=invitations`;
  const rows = invitations.data?.filter((i) => matches(search, i.invitee, i.invitee_type) && (!status || i.status === status));
  return (
    <Page
      eyebrow="Organization"
      title="Invitations"
      sub="Email and handle invitations expire and become attested memberships."
      actions={
        <Button variant="primary" to={invite}>
          <UserPlus size={14} aria-hidden /> Invite someone
        </Button>
      }
    >
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search invitations…" />
        <FilterChips options={countBy(invitations.data, (i) => i.status)} value={status} onChange={setStatus} allLabel="Any status" />
      </Toolbar>
      <DataTable
        q={invitations}
        what="invitations"
        rows={rows}
        rowKey={(i) => i.id}
        empty={
          <PanelEmpty title="No invitations" action={<Button to={invite}>Invite someone</Button>}>
            {invitations.data?.length ? "Clear the search or the status filter to see more." : "Invite someone by email address or handle."}
          </PanelEmpty>
        }
        columns={[
          { key: "invitee", label: "Invitee", sort: (i) => i.invitee, render: (i) => <NameCell name={i.invitee} sub={i.invitee_type} /> },
          { key: "role", label: "Role", render: (i) => <RoleBadge role={i.role} /> },
          { key: "status", label: "Status", render: (i) => <Status value={i.status} /> },
          { key: "expires", label: "Expires", render: (i) => <When value={i.expires_at} />, sort: (i) => i.expires_at },
        ]}
      />
    </Page>
  );
}
