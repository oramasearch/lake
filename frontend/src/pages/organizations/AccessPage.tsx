/** Effective Access: who can reach one object, with which actions, and from where. One renderer
 * (web_ui_effective_access) behind two kinds of route: `/access?object_id=` names a policy object
 * (serve_access_page); `/repository/access`, `/organization/access` and `/team/access` take `?id=`
 * of that entity (serve_entity_access_page). */
import { useState } from "react";
import { Link, Navigate, useLocation } from "react-router-dom";
import type { UseQueryResult } from "@tanstack/react-query";
import { ShieldCheck } from "lucide-react";
import { useRead, type Row } from "@/lib/api";
import { Button, Chip, DataTable, ErrorBlock, FilterChips, Mono, Page, SearchInput, Stat, StatRow, Toolbar } from "@/lib/ui";
import { PanelEmpty, RoleBadge, countBy, matches, useMetaTrail, useParam } from "./shared";

const ENTITY: Record<string, string> = {
  "/repository/access": "repository",
  "/organization/access": "organization",
  "/team/access": "team",
};

export default function AccessPage() {
  const { pathname } = useLocation();
  const entityType = ENTITY[pathname.replace(/^\/w\/[^/]+/, "")];
  const objectId = useParam("object_id");
  const id = useParam("id");
  const byObject = !entityType;
  // /access: the object, then its effective access. The entity routes: one call answers both.
  const object = useRead<Row>(byObject && objectId ? "get_object" : null, { id: objectId });
  const direct = useRead<Row>(byObject && objectId ? "effective_access" : null, { object_id: objectId });
  const entity = useRead<Row>(!byObject && id ? "entity_effective_access" : null, { object_type: entityType, id });
  const shown: Row | undefined = byObject ? object.data : entity.data?.object;
  useMetaTrail(shown ? `${shown.object_type}:${shown.external_id}` : null, "Effective Access");
  if (byObject ? !objectId : !id) return <Navigate to="/organizations" replace />;
  const failed = [object, direct, entity].find((q) => q.isError);
  const entries = byObject ? direct : entity;
  return (
    <Page
      eyebrow="Organization"
      title="Effective Access"
      sub={
        shown ? (
          <>
            Who can access <Chip>{shown.object_type}</Chip> <code>{shown.external_id}</code>, what they can do, and where it comes from.
          </>
        ) : (
          "Who can access this object, what they can do, and where it comes from."
        )
      }
      actions={shown?.id ? <Button to={`/grants?object_id=${encodeURIComponent(shown.id)}`}>Direct grants</Button> : undefined}
    >
      {failed ? <ErrorBlock error={failed.error} retry={() => void failed.refetch()} what="effective access" /> : <AccessTable q={entries} />}
    </Page>
  );
}

function AccessTable({ q }: { q: UseQueryResult<Row> }) {
  const [search, setSearch] = useState("");
  const [role, setRole] = useState<string | null>(null);
  const all = q.data?.items as Row[] | undefined;
  const rows = all?.filter((e) => matches(search, e.principal_taap_id, e.source_object_id) && (!role || e.role === role));
  return (
    <>
      {all && all.length > 0 && (
        <StatRow cols={3}>
          <Stat label="Principals" value={new Set(all.map((e) => e.principal_taap_id)).size} icon={ShieldCheck} />
          <Stat label="Direct" value={all.filter((e) => !e.inherited).length} sub="granted on this object" />
          <Stat label="Inherited" value={all.filter((e) => e.inherited).length} sub="from the scopes above it" />
        </StatRow>
      )}
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search principals…" />
        <FilterChips options={countBy(all, (e) => e.role)} value={role} onChange={setRole} allLabel="Any role" />
      </Toolbar>
      <DataTable
        q={q}
        what="effective access"
        rows={rows}
        empty={<PanelEmpty title="No effective grants">{all?.length ? "Clear the search or the role filter to see more." : "Nobody has access to this object yet."}</PanelEmpty>}
        columns={[
          { key: "principal", label: "Principal", sort: (e) => e.principal_taap_id, render: (e) => <Mono short={40}>{e.principal_taap_id}</Mono> },
          { key: "role", label: "Role", sort: (e) => e.role, render: (e) => <RoleBadge role={e.role} /> },
          {
            key: "actions",
            label: "Actions",
            render: (e) => (
              <span className="flex flex-wrap gap-1">
                {((e.actions ?? []) as string[]).map((a) => (
                  <Chip key={a}>{a}</Chip>
                ))}
              </span>
            ),
          },
          {
            key: "source",
            label: "Source",
            render: (e) => (
              <span className="text-[12.5px] text-ink-2">
                {e.inherited ? "inherited from" : "direct on"}{" "}
                {e.source_object_id ? (
                  <Link to={`/grants?object_id=${encodeURIComponent(e.source_object_id)}`} title="The grants on it, to revoke one">
                    <Mono short={16}>{e.source_object_id}</Mono>
                  </Link>
                ) : null}
              </span>
            ),
          },
        ]}
      />
    </>
  );
}
