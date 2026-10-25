/** Customers (`/customers`): the tenants, each an individual's or an organization's
 * (web_ui_customers, serve_customers_page). The operator gives a credit from Orama here by account
 * ID or a person's email (Give credit, lake#1134), where billing is on. */
import { useState } from "react";
import { Building2, Gift, User, Users } from "lucide-react";
import { useList, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { GiveCredit } from "@/pages/billing/GiveCredit";
import { Button, Chip, DataTable, FilterChips, Mono, Page, SearchInput, Stat, StatRow, Toolbar } from "@/lib/ui";
import { NameCell, PanelEmpty, When, countBy, matches, useMetaTrail } from "@/pages/organizations/shared";

export default function CustomersPage() {
  const customers = useList<Row>("list_customers", { limit: 500 }, { pick: "items" });
  const [search, setSearch] = useState("");
  const [type, setType] = useState<string | null>(null);
  useMetaTrail(customers.data ? `${customers.data.length} customers` : null, "Customers");
  const all = customers.data;
  const rows = all?.filter((c) => matches(search, c.name, c.user_id, c.organization_id, c.id) && (!type || c.customer_type === type));
  const of = (t: string) => (all ?? []).filter((c) => c.customer_type === t).length;
  const { viewer } = useSession();
  const [giving, setGiving] = useState(false);
  return (
    <Page
      eyebrow="Administration"
      title="Customers"
      sub="The tenants. Each is an individual or an organization and owns its repositories."
      actions={
        viewer.administrator && viewer.billing ? (
          <Button onClick={() => setGiving(true)}>
            <Gift size={14} aria-hidden /> Give credit
          </Button>
        ) : undefined
      }
    >
      {giving && <GiveCredit onClose={() => setGiving(false)} />}
      {all && all.length > 0 && (
        <StatRow cols={3}>
          <Stat label="Customers" value={all.length} icon={Users} />
          <Stat label="Individuals" value={of("individual")} icon={User} />
          <Stat label="Organizations" value={of("organization")} icon={Building2} />
        </StatRow>
      )}
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search customers…" />
        <FilterChips options={countBy(all, (c) => c.customer_type)} value={type} onChange={setType} allLabel="Any type" />
      </Toolbar>
      <DataTable
        q={customers}
        what="customers"
        rows={rows}
        rowKey={(c) => c.id}
        empty={
          <PanelEmpty title="No customers">
            {all?.length ? "Clear the search or the type filter to see more." : "No customers yet. A customer appears when a person signs up or creates an organization."}
          </PanelEmpty>
        }
        columns={[
          {
            key: "handle",
            label: "Handle",
            sort: (c) => c.name,
            render: (c) => <NameCell to={`/repositories?customer_id=${encodeURIComponent(c.id)}`} name={c.name} sub={<Mono short={36}>{c.customer_type === "individual" ? c.user_id : c.organization_id}</Mono>} />,
          },
          { key: "type", label: "Type", sort: (c) => c.customer_type, render: (c) => <Chip tone={c.customer_type === "organization" ? "action" : "neutral"}>{c.customer_type}</Chip> },
          { key: "created", label: "Created", render: (c) => <When value={c.created_at} />, sort: (c) => c.created_at },
        ]}
      />
    </Page>
  );
}
