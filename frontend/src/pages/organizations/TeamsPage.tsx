// SPDX-License-Identifier: MIT

/** Teams (`/teams?organization_id=`): an organization's teams (web_ui_teams, serve_teams_page). */
import { useState } from "react";
import { Link, Navigate } from "react-router-dom";
import { Plus } from "lucide-react";
import { useList, type Row } from "@/lib/api";
import { Button, DataTable, Page, SearchInput, Toolbar } from "@/lib/ui";
import { NameCell, PanelEmpty, When, matches, useMetaTrail, useParam } from "./shared";

export default function TeamsPage() {
  const organizationId = useParam("organization_id");
  const teams = useList<Row>(organizationId ? "list_teams" : null, { organization_id: organizationId, limit: 500 }, { pick: "items" });
  const [search, setSearch] = useState("");
  useMetaTrail(organizationId || null, "Teams");
  if (!organizationId) return <Navigate to="/organizations" replace />;
  const org = encodeURIComponent(organizationId);
  const create = `/organization?id=${org}&tab=teams`;
  const rows = teams.data?.filter((t) => matches(search, t.name, t.display_name));
  return (
    <Page
      eyebrow="Organization"
      title="Teams"
      sub="Roles on an organization flow to its teams unless inheritance is cut."
      actions={
        <Button variant="primary" to={create}>
          <Plus size={14} aria-hidden /> New team
        </Button>
      }
    >
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search teams…" />
      </Toolbar>
      <DataTable
        q={teams}
        what="teams"
        rows={rows}
        rowKey={(t) => t.id}
        empty={
          <PanelEmpty title="No teams" action={<Button to={create}>New team</Button>}>
            {teams.data?.length ? "Clear the search to see every team." : "No teams yet. Create one from the organization's settings."}
          </PanelEmpty>
        }
        columns={[
          { key: "team", label: "Team", sort: (t) => t.display_name || t.name, render: (t) => <NameCell to={`/team?id=${encodeURIComponent(t.id)}`} name={t.display_name || t.name} sub={<code>{t.name}</code>} /> },
          {
            key: "links",
            label: <span className="sr-only">Links</span>,
            render: (t) => (
              <span className="flex flex-wrap gap-3 text-[12.5px]">
                <Link to={`/members?organization_id=${encodeURIComponent(t.organization_id)}&team_id=${encodeURIComponent(t.id)}`}>members</Link>
                <Link to={`/team/access?id=${encodeURIComponent(t.id)}`}>effective access</Link>
              </span>
            ),
          },
          { key: "created", label: "Created", render: (t) => <When value={t.created_at} />, sort: (t) => t.created_at },
        ]}
      />
    </Page>
  );
}
