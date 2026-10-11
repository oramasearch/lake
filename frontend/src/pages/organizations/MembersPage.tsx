/** Members (`/members?organization_id=&team_id=`): an organization's or team's memberships with
 * their attestations (web_ui_memberships, serve_members_page). */
import { useState } from "react";
import { Navigate } from "react-router-dom";
import { UserPlus } from "lucide-react";
import { useList, type Row } from "@/lib/api";
import { Button, DataTable, FilterChips, Mono, Page, SearchInput, Toolbar } from "@/lib/ui";
import { NameCell, PanelEmpty, RoleBadge, countBy, matches, useMetaTrail, useParam } from "./shared";

export default function MembersPage() {
  const organizationId = useParam("organization_id");
  const teamId = useParam("team_id");
  const memberships = useList<Row>(organizationId ? "list_memberships" : null, { organization_id: organizationId, team_id: teamId, limit: 500 }, { pick: "items" });
  const [search, setSearch] = useState("");
  const [role, setRole] = useState<string | null>(null);
  useMetaTrail(teamId || organizationId || null, "Members");
  if (!organizationId) return <Navigate to="/organizations" replace />;
  const invite = `/organization?id=${encodeURIComponent(organizationId)}&tab=invitations`;
  const rows = memberships.data?.filter((m) => matches(search, m.principal_taap_id, m.principal_type, m.granted_by_taap_id) && (!role || m.role === role));
  return (
    <Page
      eyebrow="Organization"
      title="Members"
      sub="Every role is backed by an org_membership/1 attestation."
      actions={
        <Button variant="primary" to={invite}>
          <UserPlus size={14} aria-hidden /> Invite people
        </Button>
      }
    >
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search members…" />
        <FilterChips options={countBy(memberships.data, (m) => m.role)} value={role} onChange={setRole} allLabel="Any role" />
      </Toolbar>
      <DataTable
        q={memberships}
        what="members"
        rows={rows}
        rowKey={(m) => m.id}
        empty={
          <PanelEmpty title="No members" action={<Button to={invite}>Invite people</Button>}>
            {memberships.data?.length ? "Clear the search or the role filter to see more." : "No members yet. Invite people to add them."}
          </PanelEmpty>
        }
        columns={[
          {
            key: "principal",
            label: "Member",
            sort: (m) => m.principal_taap_id,
            render: (m) => <NameCell to={`/member?id=${encodeURIComponent(m.id)}`} name={<Mono short={34}>{m.principal_taap_id}</Mono>} sub={m.principal_type} />,
          },
          { key: "role", label: "Role", sort: (m) => m.role, render: (m) => <RoleBadge role={m.role} /> },
          { key: "attestation", label: "Attestation", render: (m) => <Mono short={14}>{m.attestation_digest}</Mono> },
          { key: "granted", label: "Granted by", render: (m) => <Mono short={28}>{m.granted_by_taap_id}</Mono> },
        ]}
      />
    </Page>
  );
}
