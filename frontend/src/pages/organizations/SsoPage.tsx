/** Enterprise SSO (`/sso?organization_id=&provider_id=`): an organization's providers, the issuer
 * claims it allows and one provider's SCIM provisioning log (web_ui_sso, serve_sso_page). */
import { useState } from "react";
import { Link, Navigate } from "react-router-dom";
import { Fingerprint, KeyRound, Plus, ScrollText } from "lucide-react";
import { useList, type Row } from "@/lib/api";
import { Banner, Button, Card, DataTable, FilterChips, Page, Stat, StatRow, Status, Toolbar, cx } from "@/lib/ui";
import { NameCell, PanelEmpty, When, countBy, useMetaTrail, useParam } from "./shared";

export default function SsoPage() {
  const organizationId = useParam("organization_id");
  const asked = useParam("provider_id");
  const providers = useList<Row>(organizationId ? "list_sso_providers" : null, { organization_id: organizationId, limit: 100 }, { pick: "items" });
  const issuers = useList<Row>(organizationId ? "list_federation_issuers" : null, { organization_id: organizationId, limit: 500 }, { pick: "items" });
  // The provider whose log is shown: the one asked for, else the first.
  const providerId = asked || providers.data?.[0]?.id || "";
  const logs = useList<Row>(providerId ? "list_sso_provisioning_logs" : null, { provider_id: providerId, limit: 100 }, { pick: "items" });
  const [outcome, setOutcome] = useState<string | null>(null);
  useMetaTrail(organizationId ? `organization ${organizationId}` : null, "Enterprise SSO");
  if (!organizationId) return <Navigate to="/organizations" replace />;
  const org = encodeURIComponent(organizationId);
  const add = `/organization?id=${org}&tab=sso`;
  const provider = providers.data?.find((p) => p.id === providerId);
  const logRows = logs.data?.filter((l) => !outcome || l.outcome === outcome);
  return (
    <Page
      eyebrow="Organization"
      title="Enterprise SSO"
      sub="Provider configuration, federation policy, and SCIM provisioning evidence."
      actions={
        <Button variant="primary" to={add}>
          <Plus size={14} aria-hidden /> Add a provider
        </Button>
      }
    >
      <Banner tone="quiet">
        Add a provider from the <Link to={add}>organization's settings</Link>. Provider secrets and SCIM tokens are never shown here.
      </Banner>
      <StatRow cols={3}>
        <Stat label="Providers" value={providers.data ? providers.data.length : "—"} icon={KeyRound} sub={providers.data ? `${providers.data.filter((p) => p.tested_at).length} tested` : undefined} />
        <Stat label="Issuer claims" value={issuers.data ? issuers.data.length : "—"} icon={Fingerprint} sub="allowed for federation" />
        <Stat label="Provisioning events" value={logs.data ? logs.data.length : "—"} icon={ScrollText} sub={provider ? `for ${provider.name}` : "no provider chosen"} />
      </StatRow>
      <Card label="Providers" padded={false} className="mb-4">
        <DataTable
          q={providers}
          what="SSO providers"
          rows={providers.data}
          rowKey={(p) => p.id}
          empty={
            <div className="p-4">
              <PanelEmpty title="No SSO providers" action={<Button to={add}>Add a provider</Button>}>
                No SSO providers are configured.
              </PanelEmpty>
            </div>
          }
          columns={[
            {
              key: "name",
              label: "Provider",
              // Choosing a row shows its provisioning log (the server read provider_id from the URL only).
              render: (p) => (
                <div className={cx(p.id === providerId && "border-l-[3px] border-l-action pl-2")}>
                  <NameCell
                    name={
                      <Link to={`/sso?organization_id=${org}&provider_id=${encodeURIComponent(p.id)}`} aria-current={p.id === providerId ? "true" : undefined}>
                        <code>{p.name}</code>
                      </Link>
                    }
                    sub={p.issuer}
                  />
                </div>
              ),
            },
            { key: "protocol", label: "Protocol", render: (p) => <span className="font-mono text-[12px] uppercase">{p.protocol}</span> },
            { key: "jit", label: "JIT", render: (p) => <Status value={p.jit_provisioning ? "enabled" : "disabled"} tone={p.jit_provisioning ? "success" : "neutral"} /> },
            { key: "tested", label: "Tested", render: (p) => (p.tested_at ? <When value={p.tested_at} /> : <Status value="not tested" tone="warning" />) },
          ]}
        />
      </Card>
      <Card label="Allowed issuer claims" padded={false} className="mb-4">
        <DataTable
          q={issuers}
          what="issuer claims"
          rows={issuers.data}
          rowKey={(i) => i.id}
          empty={
            <div className="p-4">
              <PanelEmpty>No issuer claims are allowed yet.</PanelEmpty>
            </div>
          }
          columns={[
            { key: "issuer", label: "Issuer", render: (i) => <code>{i.issuer}</code> },
            { key: "claim", label: "Claim", render: (i) => <code>{i.claim}</code> },
          ]}
        />
      </Card>
      <Card label="Provisioning log" title={provider ? <code>{provider.name}</code> : undefined} padded={false}>
        <div className="px-4 pt-3">
          <Toolbar>
            <FilterChips options={countBy(logs.data, (l) => l.outcome)} value={outcome} onChange={setOutcome} allLabel="Any outcome" />
          </Toolbar>
        </div>
        {providerId ? (
          <DataTable
            q={logs}
            what="the provisioning log"
            rows={logRows}
            rowKey={(l) => l.id}
            empty={
              <div className="px-4 pb-4">
                <PanelEmpty>No SCIM provisioning events for this provider.</PanelEmpty>
              </div>
            }
            columns={[
              { key: "when", label: "When", sort: (l) => l.created_at, render: (l) => <When value={l.created_at} /> },
              { key: "resource", label: "Resource", render: (l) => <NameCell name={l.resource} sub={l.operation} /> },
              { key: "external", label: "External id", render: (l) => <code>{l.external_id}</code> },
              { key: "outcome", label: "Outcome", render: (l) => <Status value={l.outcome} /> },
            ]}
          />
        ) : (
          !providers.isLoading && (
            <div className="px-4 pb-4">
              <PanelEmpty>No SCIM provisioning events for this provider.</PanelEmpty>
            </div>
          )
        )}
      </Card>
    </Page>
  );
}
