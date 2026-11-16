// SPDX-License-Identifier: MIT

/** Team settings (`/team?id=`): the team's members and how many access entries it has
 * (web_ui_team, serve_team_page). */
import { useState } from "react";
import { Link } from "react-router-dom";
import { ShieldCheck } from "lucide-react";
import { useList, useRead, type Row } from "@/lib/api";
import { Button, Card, DataTable, ErrorBlock, KeyValue, Mono, Page, SearchInput, Skeleton, Split, Toolbar } from "@/lib/ui";
import { NameCell, PanelEmpty, QueryTabs, RoleBadge, When, matches, useMetaTrail, useParam } from "./shared";

export default function TeamPage() {
  const id = useParam("id");
  const team = useRead<Row>("get_team", { id });
  const organizationId: string | undefined = team.data?.organization_id;
  const memberships = useList<Row>(organizationId ? "list_memberships" : null, { organization_id: organizationId, team_id: id, limit: 500 }, { pick: "items" });
  const access = useRead<Row>("entity_effective_access", { object_type: "team", id });
  const organization = useRead<Row>(organizationId ? "get_organization" : null, { id: organizationId }, { retry: false });
  const [search, setSearch] = useState("");
  useMetaTrail(team.data?.name, "Team Settings");

  if (team.isError) {
    return (
      <Page eyebrow="Organization" title="Team Settings">
        <ErrorBlock error={team.error} retry={() => void team.refetch()} what="the team" />
      </Page>
    );
  }
  if (!team.data) {
    return (
      <Page eyebrow="Organization" title="Team Settings">
        <Skeleton lines={5} />
      </Page>
    );
  }
  const t = team.data;
  const teamId = encodeURIComponent(t.id);
  const membersTo = `/members?organization_id=${encodeURIComponent(t.organization_id)}&team_id=${teamId}`;
  const rows = memberships.data?.filter((m) => matches(search, m.principal_taap_id, m.role));
  return (
    <Page
      eyebrow="Organization"
      title={t.display_name || t.name}
      sub={
        <>
          Team <code>{t.name}</code>
          {organization.data ? (
            <>
              {" "}
              in <Link to={`/organization?id=${encodeURIComponent(t.organization_id)}`}>{organization.data.display_name}</Link>
            </>
          ) : null}
        </>
      }
      actions={
        <Button to={`/team/access?id=${teamId}`}>
          <ShieldCheck size={14} aria-hidden /> Effective access
        </Button>
      }
    >
      <QueryTabs
        label="Team settings"
        tabs={[
          { to: `/team?id=${teamId}`, label: "Overview", active: true },
          { to: membersTo, label: "Members", active: false, count: memberships.data?.length },
          { to: `/team/access?id=${teamId}`, label: "Access", active: false },
        ]}
      />
      <Split
        side={
          <Card label="Team">
            <KeyValue
              rows={[
                ["Handle", <code>{t.name}</code>],
                ["Organization", organization.data ? <Link to={`/organization?id=${encodeURIComponent(t.organization_id)}`}>{organization.data.display_name}</Link> : <Mono short={16}>{t.organization_id}</Mono>],
                ["Members", memberships.data ? memberships.data.length : "…"],
                [
                  "Effective access",
                  access.isError ? <ErrorBlock error={access.error} retry={() => void access.refetch()} what="effective access" /> : access.data ? `${(access.data.items ?? []).length} access entries.` : "…",
                ],
                ["Created", t.created_at ? <When value={t.created_at} /> : null],
              ]}
            />
          </Card>
        }
      >
        <Card label="Members" padded={false} actions={<Link to={membersTo} className="text-[12.5px]">All members →</Link>}>
          <div className="px-4 pt-3">
            <Toolbar>
              <SearchInput value={search} onChange={setSearch} placeholder="Search members…" />
            </Toolbar>
          </div>
          <DataTable
            q={memberships}
            what="team members"
            rows={rows}
            rowKey={(m) => m.id}
            empty={
              <div className="px-4 pb-4">
                <PanelEmpty title="No team members">Members join a team from the organization's members.</PanelEmpty>
              </div>
            }
            columns={[
              { key: "principal", label: "Member", render: (m) => <NameCell to={`/member?id=${encodeURIComponent(m.id)}`} name={<Mono short={34}>{m.principal_taap_id}</Mono>} sub={m.principal_type} /> },
              { key: "role", label: "Role", render: (m) => <RoleBadge role={m.role} /> },
            ]}
          />
        </Card>
      </Split>
    </Page>
  );
}
