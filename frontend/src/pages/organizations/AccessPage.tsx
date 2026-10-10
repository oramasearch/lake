/** Effective Access: who can reach one object, with which actions, and from where. One renderer
 * (web_ui_effective_access) behind two kinds of route: `/access?object_id=` names a policy object
 * (serve_access_page); `/repository/access`, `/organization/access` and `/team/access` take `?id=`
 * of that entity (serve_entity_access_page). */
import { Navigate, useLocation } from "react-router-dom";
import type { UseQueryResult } from "@tanstack/react-query";
import { useRead, type Row } from "@/lib/api";
import { DataTable, ErrorBlock, Page } from "@/lib/ui";
import { PanelEmpty, RoleBadge, useMetaTrail, useParam } from "./shared";

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
    <Page eyebrow="sharing" title="Effective Access" sub="Who can access this object, what they can do, and where it comes from.">
      {failed ? (
        <ErrorBlock error={failed.error} retry={() => void failed.refetch()} what="effective access" />
      ) : (
        <AccessTable q={entries} />
      )}
    </Page>
  );
}

function AccessTable({ q }: { q: UseQueryResult<Row> }) {
  return (
    <DataTable
      q={q}
      what="effective access"
      rows={q.data?.items as Row[] | undefined}
      empty={<PanelEmpty>No effective grants.</PanelEmpty>}
      columns={[
        { key: "principal", label: "Principal", render: (e) => <code>{e.principal_taap_id}</code> },
        { key: "role", label: "Role", render: (e) => <RoleBadge role={e.role} /> },
        { key: "actions", label: "Actions", render: (e) => ((e.actions ?? []) as string[]).join(", ") },
        {
          key: "source",
          label: "Source",
          render: (e) => (
            <>
              {e.inherited ? "inherited from" : "direct on"} <code>{e.source_object_id}</code>
            </>
          ),
        },
      ]}
    />
  );
}
