/** Direct Grants (`/grants?object_id=`): the roles granted directly on one policy object
 * (web_ui_grants, serve_grants_page). */
import { useState } from "react";
import { Navigate } from "react-router-dom";
import { ShieldCheck } from "lucide-react";
import { useList, useRead, type Row } from "@/lib/api";
import { Button, Chip, DataTable, ErrorBlock, FilterChips, Mono, Page, SearchInput, Toolbar } from "@/lib/ui";
import { PanelEmpty, RoleBadge, countBy, matches, useMetaTrail, useParam } from "./shared";

export default function GrantsPage() {
  const objectId = useParam("object_id");
  const object = useRead<Row>(objectId ? "get_object" : null, { id: objectId });
  const grants = useList<Row>(objectId ? "list_grants" : null, { object_id: objectId, limit: 500 }, { pick: "items" });
  const [search, setSearch] = useState("");
  const [role, setRole] = useState<string | null>(null);
  useMetaTrail(object.data ? `${object.data.object_type}:${object.data.external_id}` : null, "Direct Grants");
  if (!objectId) return <Navigate to="/organizations" replace />;
  const rows = grants.data?.filter((g) => matches(search, g.principal_taap_id, g.granted_by_taap_id) && (!role || g.role === role));
  return (
    <Page
      eyebrow="Organization"
      title="Direct Grants"
      sub={
        object.data ? (
          <>
            Attestation-backed roles attached directly to <Chip>{object.data.object_type}</Chip> <code>{object.data.external_id}</code>.
          </>
        ) : (
          "Attestation-backed roles directly attached to this object."
        )
      }
      actions={
        <Button to={`/access?object_id=${encodeURIComponent(object.data?.id ?? objectId)}`}>
          <ShieldCheck size={14} aria-hidden /> View effective access
        </Button>
      }
    >
      {object.isError ? (
        <ErrorBlock error={object.error} retry={() => void object.refetch()} what="the object" />
      ) : (
        <>
          <Toolbar>
            <SearchInput value={search} onChange={setSearch} placeholder="Search principals…" />
            <FilterChips options={countBy(grants.data, (g) => g.role)} value={role} onChange={setRole} allLabel="Any role" />
          </Toolbar>
          <DataTable
            q={grants}
            what="grants"
            rows={rows}
            rowKey={(g) => g.id}
            empty={<PanelEmpty title="No direct grants">No direct grants on this object. Access to it comes from the workspace, organization or team above it.</PanelEmpty>}
            columns={[
              { key: "principal", label: "Principal", sort: (g) => g.principal_taap_id, render: (g) => <Mono short={40}>{g.principal_taap_id}</Mono> },
              { key: "role", label: "Role", sort: (g) => g.role, render: (g) => <RoleBadge role={g.role} /> },
              { key: "attestation", label: "Attestation", render: (g) => <Mono short={14}>{g.attestation_digest}</Mono> },
              { key: "granted", label: "Granted by", render: (g) => <Mono short={28}>{g.granted_by_taap_id}</Mono> },
            ]}
          />
        </>
      )}
    </Page>
  );
}
