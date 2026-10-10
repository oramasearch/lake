/** Teams (`/teams?organization_id=`): an organization's teams (web_ui_teams, serve_teams_page). */
import { Link, Navigate } from "react-router-dom";
import { useList, type Row } from "@/lib/api";
import { DataTable, Page } from "@/lib/ui";
import { Dotted, PanelEmpty, When, useMetaTrail, useParam } from "./shared";

export default function TeamsPage() {
  const organizationId = useParam("organization_id");
  const teams = useList<Row>(organizationId ? "list_teams" : null, { organization_id: organizationId, limit: 500 }, { pick: "items" });
  useMetaTrail(organizationId || null, "Teams");
  if (!organizationId) return <Navigate to="/organizations" replace />;
  const org = encodeURIComponent(organizationId);
  return (
    <Page eyebrow="organization" title="Teams" sub="Roles on an organization flow to its teams unless inheritance is cut.">
      <DataTable
        q={teams}
        what="teams"
        rows={teams.data}
        rowKey={(t) => t.id}
        empty={
          <PanelEmpty>
            No teams yet. <Link to={`/organization?id=${org}&tab=teams`}>Create a team</Link> from the organization's settings.
          </PanelEmpty>
        }
        columns={[
          { key: "team", label: "Team", render: (t) => t.display_name || t.name },
          { key: "name", label: "Name", render: (t) => <code>{t.name}</code> },
          {
            key: "members",
            label: "Members",
            render: (t) => (
              <Dotted>
                {[
                  <Link key="m" to={`/members?organization_id=${encodeURIComponent(t.organization_id)}&team_id=${encodeURIComponent(t.id)}`}>
                    members
                  </Link>,
                  <Link key="a" to={`/team/access?id=${encodeURIComponent(t.id)}`}>
                    effective access
                  </Link>,
                ]}
              </Dotted>
            ),
          },
          { key: "created", label: "Created", render: (t) => <When value={t.created_at} />, sort: (t) => t.created_at },
        ]}
      />
    </Page>
  );
}
