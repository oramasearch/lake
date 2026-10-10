/** Direct Grants (`/grants?object_id=`): the roles granted directly on one policy object
 * (web_ui_grants, serve_grants_page). */
import { Link, Navigate } from "react-router-dom";
import { useList, useRead, type Row } from "@/lib/api";
import { DataTable, ErrorBlock, Page } from "@/lib/ui";
import { PanelEmpty, RoleBadge, useMetaTrail, useParam } from "./shared";

export default function GrantsPage() {
  const objectId = useParam("object_id");
  const object = useRead<Row>(objectId ? "get_object" : null, { id: objectId });
  const grants = useList<Row>(objectId ? "list_grants" : null, { object_id: objectId, limit: 500 }, { pick: "items" });
  useMetaTrail(object.data ? `${object.data.object_type}:${object.data.external_id}` : null, "Direct Grants");
  if (!objectId) return <Navigate to="/organizations" replace />;
  return (
    <Page eyebrow="policy" title="Direct Grants" sub="Attestation-backed roles directly attached to this object.">
      {object.isError ? (
        <ErrorBlock error={object.error} retry={() => void object.refetch()} what="the object" />
      ) : (
        <>
          <DataTable
            q={grants}
            what="grants"
            rows={grants.data}
            rowKey={(g) => g.id}
            empty={<PanelEmpty>No direct grants on this object. Access to it comes from the workspace, organization or team above it.</PanelEmpty>}
            columns={[
              { key: "principal", label: "Principal", render: (g) => <code>{g.principal_taap_id}</code> },
              { key: "role", label: "Role", render: (g) => <RoleBadge role={g.role} /> },
              { key: "attestation", label: "Attestation", render: (g) => <code>{g.attestation_digest}</code> },
              { key: "granted", label: "Granted by", render: (g) => <code>{g.granted_by_taap_id}</code> },
            ]}
          />
          <p>
            <Link to={`/access?object_id=${encodeURIComponent(object.data?.id ?? objectId)}`}>View effective access</Link>
          </p>
        </>
      )}
    </Page>
  );
}
