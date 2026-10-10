/** Customers (`/customers`): the spaces, each an individual's or an organization's
 * (web_ui_customers, serve_customers_page). */
import { Link } from "react-router-dom";
import { useList, type Row } from "@/lib/api";
import { Badge, DataTable, Page } from "@/lib/ui";
import { PanelEmpty, When, useMetaTrail } from "@/pages/organizations/shared";

export default function CustomersPage() {
  const customers = useList<Row>("list_customers", { limit: 500 }, { pick: "items" });
  useMetaTrail(customers.data ? `${customers.data.length} customers` : null, "Customers");
  return (
    <Page eyebrow="control plane" title="Customers" sub="The tenants. Each is an individual or an organization and owns its repositories.">
      <DataTable
        q={customers}
        what="customers"
        rows={customers.data}
        rowKey={(c) => c.id}
        empty={<PanelEmpty>No customers yet. A customer appears when a person signs up or creates an organization.</PanelEmpty>}
        columns={[
          { key: "handle", label: "Handle", render: (c) => <Link to={`/repositories?customer_id=${encodeURIComponent(c.id)}`}>{c.name}</Link>, sort: (c) => c.name },
          { key: "type", label: "Type", render: (c) => <Badge>{c.customer_type}</Badge> },
          { key: "identity", label: "Identity", render: (c) => <code>{c.customer_type === "individual" ? c.user_id : c.organization_id}</code> },
          { key: "created", label: "Created", render: (c) => <When value={c.created_at} />, sort: (c) => c.created_at },
        ]}
      />
    </Page>
  );
}
