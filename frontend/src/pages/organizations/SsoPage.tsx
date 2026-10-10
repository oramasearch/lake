/** Enterprise SSO (`/sso?organization_id=&provider_id=`): an organization's providers, the issuer
 * claims it allows and one provider's SCIM provisioning log (web_ui_sso, serve_sso_page). */
import { Link, Navigate } from "react-router-dom";
import { useList, type Row } from "@/lib/api";
import { DataTable, Page, Status } from "@/lib/ui";
import { Panel, PanelEmpty, When, useMetaTrail, useParam } from "./shared";

export default function SsoPage() {
  const organizationId = useParam("organization_id");
  const asked = useParam("provider_id");
  const providers = useList<Row>(organizationId ? "list_sso_providers" : null, { organization_id: organizationId, limit: 100 }, { pick: "items" });
  const issuers = useList<Row>(organizationId ? "list_federation_issuers" : null, { organization_id: organizationId, limit: 500 }, { pick: "items" });
  // The provider whose log is shown: the one asked for, else the first.
  const providerId = asked || providers.data?.[0]?.id || "";
  const logs = useList<Row>(providerId ? "list_sso_provisioning_logs" : null, { provider_id: providerId, limit: 100 }, { pick: "items" });
  useMetaTrail(organizationId ? `organization ${organizationId}` : null, "Enterprise SSO");
  if (!organizationId) return <Navigate to="/organizations" replace />;
  const org = encodeURIComponent(organizationId);
  return (
    <Page eyebrow="organization identity" title="Enterprise SSO" sub="Provider configuration, federation policy, and SCIM provisioning evidence.">
      <Panel>
        <p>
          <Link to={`/organization?id=${org}&tab=sso`}>Add a provider</Link> from the organization's settings. Provider secrets and SCIM tokens are never shown here.
        </p>
      </Panel>
      <h2>Providers</h2>
      <DataTable
        q={providers}
        what="SSO providers"
        rows={providers.data}
        rowKey={(p) => p.id}
        empty={<PanelEmpty>No SSO providers are configured.</PanelEmpty>}
        columns={[
          {
            key: "name",
            label: "Name",
            // Choosing a row shows its provisioning log (the server read provider_id from the URL only).
            render: (p) => (
              <Link to={`/sso?organization_id=${org}&provider_id=${encodeURIComponent(p.id)}`} aria-current={p.id === providerId ? "true" : undefined}>
                <code>{p.name}</code>
              </Link>
            ),
          },
          { key: "protocol", label: "Protocol", render: (p) => p.protocol },
          { key: "issuer", label: "Issuer", render: (p) => <code>{p.issuer}</code> },
          { key: "jit", label: "JIT", render: (p) => (p.jit_provisioning ? "enabled" : "disabled") },
          { key: "tested", label: "Tested", render: (p) => (p.tested_at ? <When value={p.tested_at} /> : "not tested") },
        ]}
      />
      <h2>Allowed Issuer Claims</h2>
      <DataTable
        q={issuers}
        what="issuer claims"
        rows={issuers.data}
        rowKey={(i) => i.id}
        empty={<PanelEmpty>No issuer claims are allowed yet.</PanelEmpty>}
        columns={[
          { key: "issuer", label: "Issuer", render: (i) => <code>{i.issuer}</code> },
          { key: "claim", label: "Claim", render: (i) => <code>{i.claim}</code> },
        ]}
      />
      <h2>Provisioning Log</h2>
      {providerId ? (
        <DataTable
          q={logs}
          what="the provisioning log"
          rows={logs.data}
          rowKey={(l) => l.id}
          empty={<PanelEmpty>No SCIM provisioning events for this provider.</PanelEmpty>}
          columns={[
            { key: "when", label: "When", render: (l) => <When value={l.created_at} /> },
            { key: "resource", label: "Resource", render: (l) => l.resource },
            { key: "operation", label: "Operation", render: (l) => l.operation },
            { key: "external", label: "External id", render: (l) => <code>{l.external_id}</code> },
            { key: "outcome", label: "Outcome", render: (l) => <Status value={l.outcome} /> },
          ]}
        />
      ) : (
        !providers.isLoading && <PanelEmpty>No SCIM provisioning events for this provider.</PanelEmpty>
      )}
    </Page>
  );
}
