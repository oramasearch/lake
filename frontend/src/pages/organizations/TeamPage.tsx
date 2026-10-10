/** Team settings (`/team?id=`): the team's members and how many access entries it has
 * (web_ui_team, serve_team_page). */
import { Link } from "react-router-dom";
import { useList, useRead, type Row } from "@/lib/api";
import { ErrorBlock, Page, Query, Skeleton } from "@/lib/ui";
import { Panel, useMetaTrail, useParam } from "./shared";

export default function TeamPage() {
  const id = useParam("id");
  const team = useRead<Row>("get_team", { id });
  const organizationId: string | undefined = team.data?.organization_id;
  const memberships = useList<Row>(organizationId ? "list_memberships" : null, { organization_id: organizationId, team_id: id, limit: 500 }, { pick: "items" });
  const access = useRead<Row>("entity_effective_access", { object_type: "team", id });
  useMetaTrail(team.data?.name, "Team Settings");

  if (team.isError) {
    return (
      <Page eyebrow="organization settings" title="Team Settings">
        <ErrorBlock error={team.error} retry={() => void team.refetch()} what="the team" />
      </Page>
    );
  }
  if (!team.data) {
    return (
      <Page eyebrow="organization settings" title="Team Settings">
        <Skeleton />
      </Page>
    );
  }
  const t = team.data;
  const teamId = encodeURIComponent(t.id);
  return (
    <Page eyebrow="organization settings" title={t.display_name || t.name}>
      <nav className="tabs" aria-label="Team settings">
        <Link aria-current="page" to={`/team?id=${teamId}`}>
          Overview
        </Link>
        <Link to={`/members?organization_id=${encodeURIComponent(t.organization_id)}&team_id=${teamId}`}>Members</Link>
        <Link to={`/team/access?id=${teamId}`}>Access</Link>
      </nav>
      <Panel title="Members">
        <Query q={memberships} what="team members" empty={<p className="empty">No team members.</p>}>
          {(rows) => (
            <>
              {rows.map((member) => (
                <p key={member.id}>
                  <Link to={`/member?id=${encodeURIComponent(member.id)}`}>{member.principal_taap_id}</Link>
                </p>
              ))}
            </>
          )}
        </Query>
      </Panel>
      <Panel title="Effective Access">
        {access.isError ? (
          <ErrorBlock error={access.error} retry={() => void access.refetch()} what="effective access" />
        ) : (
          <p>{access.data ? `${(access.data.items ?? []).length} access entries.` : "…"}</p>
        )}
      </Panel>
    </Page>
  );
}
